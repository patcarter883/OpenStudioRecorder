// Regression test for the recorder's range DECLARATION mapping (source/range_declaration.h).
//
// A JPEG XS node that signs full range and is passed through untouched must be declared FULL on BOTH
// legs - full in, full out - and therefore tagged 1:3:5:1, which is what the patched mxfmux writes
// into the MXF. Before this mapping existed, every JPEG XS recording was declared "unknown" (0:3:5:1)
// whatever the node said, and a player reading "unknown" assumes limited range, so full-range
// samples were rendered through a limited interpretation: crushed blacks, blown whites.
//
// NULL HYPOTHESIS: these functions must FOLLOW the mode, not return a constant. The test asserts a
// DIFFERENT answer for the measured-full and measured-limited cases, so a hardcoded answer fails.
#include <catch2/catch_test_macros.hpp>

#include <string>

#include "range_declaration.h"

TEST_CASE("A measured full-range pass-through declares FULL on both legs", "[range][jxs]")
{
  const osr::RangeMode mode = osr::RangeMode::PassthroughFull;
  REQUIRE(unit_range_for(mode) == ColorRange::Full);
  REQUIRE(encoded_range_for(mode) == ColorRange::Full);
  REQUIRE(std::string(colorimetry_for(encoded_range_for(mode))) == "1:3:5:1");
  // and no conversion element may be inserted: the samples are already full.
  REQUIRE(encoded_range_for(mode) != ColorRange::Unspecified);
}

TEST_CASE("A measured limited-range pass-through declares LIMITED on both legs", "[range][jxs]")
{
  const osr::RangeMode mode = osr::RangeMode::PassthroughLimited;
  REQUIRE(unit_range_for(mode) == ColorRange::Limited);
  REQUIRE(encoded_range_for(mode) == ColorRange::Limited);
  REQUIRE(std::string(colorimetry_for(encoded_range_for(mode))) == "bt709");
}

TEST_CASE("The declaration follows the MEASURED value, not a constant", "[range][jxs]")
{
  // The same code path, two different signed values, two different declarations: a hardcoded
  // answer cannot satisfy both.
  REQUIRE(encoded_range_for(osr::RangeMode::PassthroughFull) !=
          encoded_range_for(osr::RangeMode::PassthroughLimited));
  REQUIRE(std::string(colorimetry_for(encoded_range_for(osr::RangeMode::PassthroughFull))) !=
          std::string(colorimetry_for(encoded_range_for(osr::RangeMode::PassthroughLimited))));
}

TEST_CASE("An unmeasured pass-through stays honestly unknown, never limited", "[range]")
{
  const osr::RangeMode mode = osr::RangeMode::Passthrough;
  REQUIRE(unit_range_for(mode) == ColorRange::Unspecified);
  REQUIRE(encoded_range_for(mode) == ColorRange::Unspecified);
  REQUIRE(std::string(colorimetry_for(encoded_range_for(mode))) == "0:3:5:1");
}

TEST_CASE("The conversion modes are unchanged", "[range]")
{
  REQUIRE(unit_range_for(osr::RangeMode::FullToLimited) == ColorRange::Full);
  REQUIRE(encoded_range_for(osr::RangeMode::FullToLimited) == ColorRange::Limited);
  REQUIRE(unit_range_for(osr::RangeMode::LimitedToFull) == ColorRange::Limited);
  REQUIRE(encoded_range_for(osr::RangeMode::LimitedToFull) == ColorRange::Full);
}

TEST_CASE("A measured pass-through gains no conversion element - the samples go through untouched",
          "[range][jxs]")
{
  // "full in, full out" only means what it says if nothing on the chain converts the samples: a
  // pass-through gets NO element, while a real conversion still gets its own.
  REQUIRE(range_element_for(osr::RangeMode::PassthroughFull) == nullptr);
  REQUIRE(range_element_for(osr::RangeMode::PassthroughLimited) == nullptr);
  REQUIRE(range_element_for(osr::RangeMode::Passthrough) == nullptr);
  REQUIRE(std::string(range_element_for(osr::RangeMode::FullToLimited)) ==
          "osrrange mode=full-to-limited");
  REQUIRE(std::string(range_element_for(osr::RangeMode::LimitedToFull)) ==
          "osrrange mode=limited-to-full");
}
