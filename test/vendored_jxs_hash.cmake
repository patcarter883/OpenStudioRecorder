# Verifies the vendored JPEG XS parser is BYTE-IDENTICAL to the copy it was taken from.
#
# The recorder reads the stream's colour/range declaration with the SAME parser the OBS plugin ships
# (obs-jxs-camera, branch fix/colour-range-truth, commit 2253d50): vendor/jxs/{jxs_ts.c,jxs_ts.h,
# s302m.c,s302m.h}. The stack's rule is one definition per constant - a local "improvement" to a
# vendored copy is how two parsers start to disagree - so a change to any of these files must be a
# deliberate re-copy, with these hashes updated from the source of truth.
#
# Run: cmake -DVENDOR_JXS_DIR=<repo>/vendor/jxs -P test/vendored_jxs_hash.cmake
#
# If this fails after an intentional re-copy, refresh the expected values from the plugin checkout:
#   sha256sum <plugin>/src/{jxs_ts.c,jxs_ts.h,s302m.c,s302m.h}

if(NOT DEFINED VENDOR_JXS_DIR)
  message(FATAL_ERROR "VENDOR_JXS_DIR not set")
endif()

# sha256 of each file in obs-jxs-camera@2253d50 (fix/colour-range-truth), which is also what
# vendor/jxs/VENDORED.md records.
set(expected_jxs_ts_c 938147ea6b24cae8ae6a6b3a3e7aed4d0aa933187709875cec54b2671d47e280)
set(expected_jxs_ts_h ee24ed9f509ef3e17730f104fff97061c6e8c48da28cc23b37d70ee0d2efe439)
set(expected_s302m_c  ab700097f046486dac69caf10fb3405bf5e6b1f441ab8e40effb582b67e60cfc)
set(expected_s302m_h  c41bc7d4ee0fd086e1e9cabf37a10465318908353a938537542632dccef58649)

set(failed FALSE)
foreach(fname jxs_ts.c jxs_ts.h s302m.c s302m.h)
  string(REPLACE "." "_" var "${fname}")
  set(path "${VENDOR_JXS_DIR}/${fname}")
  if(NOT EXISTS "${path}")
    message(SEND_ERROR "missing vendored file: ${path}")
    set(failed TRUE)
    continue()
  endif()
  file(SHA256 "${path}" actual)
  if(NOT actual STREQUAL "${expected_${var}}")
    message(SEND_ERROR
      "vendored ${fname} does NOT match the plugin's copy:\n"
      "  expected ${expected_${var}}\n"
      "  actual   ${actual}\n"
      "If the re-copy was deliberate, update test/vendored_jxs_hash.cmake and vendor/jxs/VENDORED.md.")
    set(failed TRUE)
  else()
    message(STATUS "vendored ${fname} matches the plugin copy (${actual})")
  endif()
endforeach()

if(failed)
  message(FATAL_ERROR "vendored JPEG XS parser hash check FAILED")
endif()
message(STATUS "vendored JPEG XS parser is byte-identical to obs-jxs-camera@2253d50")
