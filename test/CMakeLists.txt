# Parent project does not export its library target, so this CML implicitly
# depends on being added from it, i.e. the testing is done only from the build
# tree and is not feasible from an install location

project(OpenStudioRecorderTests LANGUAGES C CXX)

# ---- Dependencies ----

find_package(Catch2 REQUIRED)
include(Catch)

# ---- Tests ----

add_executable(OpenStudioRecorder_test
    source/OpenStudioRecorder_test.cpp
    source/jxs_range_declaration_test.cpp)
target_link_libraries(
    OpenStudioRecorder_test PRIVATE
    OpenStudioRecorder_lib
    Catch2::Catch2WithMain
)
target_compile_features(OpenStudioRecorder_test PRIVATE cxx_std_20)

catch_discover_tests(OpenStudioRecorder_test)

# The JPEG XS range declaration, driven through the recorder's own probe glue over the VENDORED
# parser. Hand-built MPEG-TS wire bytes (both carriage points) - no muxer, no GStreamer, no device.
add_executable(jxs_ts_colour_test
    tools/jxs_ts_colour_test.c
    ${PROJECT_SOURCE_DIR}/../source/jxs_range_probe.c
    ${PROJECT_SOURCE_DIR}/../vendor/jxs/jxs_ts.c
    ${PROJECT_SOURCE_DIR}/../vendor/jxs/s302m.c)
target_include_directories(jxs_ts_colour_test PRIVATE
    ${PROJECT_SOURCE_DIR}/../source
    ${PROJECT_SOURCE_DIR}/../vendor/jxs)
target_compile_options(jxs_ts_colour_test PRIVATE -Wall -Wextra)
add_test(NAME jxs-ts-colour COMMAND jxs_ts_colour_test)

# Field check (not a test): feed a captured .ts through the probe and print what it declares. Used
# to check a capture on disk against what the node signed, without OBS and without a camera.
add_executable(jxs_range_probe_capture
    tools/jxs_range_probe_capture.c
    ${PROJECT_SOURCE_DIR}/../source/jxs_range_probe.c
    ${PROJECT_SOURCE_DIR}/../vendor/jxs/jxs_ts.c
    ${PROJECT_SOURCE_DIR}/../vendor/jxs/s302m.c)
target_include_directories(jxs_range_probe_capture PRIVATE
    ${PROJECT_SOURCE_DIR}/../source
    ${PROJECT_SOURCE_DIR}/../vendor/jxs)
target_compile_options(jxs_range_probe_capture PRIVATE -Wall -Wextra)

# The vendored parser must stay byte-identical to the copy it came from: one definition per
# constant. Fails the test if a vendored file drifts.
add_test(NAME vendored-jxs-hash
    COMMAND ${CMAKE_COMMAND}
        -DVENDOR_JXS_DIR=${PROJECT_SOURCE_DIR}/../vendor/jxs
        -P ${CMAKE_CURRENT_SOURCE_DIR}/vendored_jxs_hash.cmake)

# ---- End-of-file commands ----

add_folders(Test)
