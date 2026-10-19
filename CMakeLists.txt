cmake_minimum_required(VERSION 3.14)
list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
include(cmake/prelude.cmake)

project(
    OpenStudioRecorder
    VERSION 0.1.0
    DESCRIPTION "Short description"
    HOMEPAGE_URL "https://example.com/"
    # C as well as CXX: the vendored discovery/controls translation units are C, as they
    # are in the plugin repo. With only CXX enabled CMake drops a .c file from the target
    # without an error - the link then fails on symbols nothing compiled.
    LANGUAGES C CXX
)

include(cmake/project-is-top-level.cmake)
include(cmake/variables.cmake)

IF (WIN32)
    add_compile_definitions(WIN32 WIN32_LEAN_AND_MEAN)
    find_package(GStreamer REQUIRED)
    find_package (Threads)

ELSE()
    find_package(PkgConfig REQUIRED)
    pkg_search_module(gstreamer REQUIRED IMPORTED_TARGET gstreamer-1.0>=1.4)
    pkg_search_module(gstreamer-net REQUIRED IMPORTED_TARGET gstreamer-net-1.0>=1.4)
    pkg_search_module(gstreamer-sdp REQUIRED IMPORTED_TARGET gstreamer-sdp-1.0>=1.4)
    pkg_search_module(gstreamer-rtp REQUIRED IMPORTED_TARGET gstreamer-rtp-1.0>=1.4)
    pkg_search_module(gstreamer-app REQUIRED IMPORTED_TARGET gstreamer-app-1.0>=1.4)
    pkg_search_module(gstreamer-video REQUIRED IMPORTED_TARGET gstreamer-video-1.0>=1.4)
    # GSocket (gio): the JXS multicast receive-socket fix injects a pre-built
    # GSocket into udpsrc (g_socket_new_from_fd); gstreamer-1.0 does not pull
    # gio into the exe's link line.
    pkg_search_module(gio REQUIRED IMPORTED_TARGET gio-2.0)
ENDIF()

find_package(NDI)
find_package(FLTK CONFIG REQUIRED)

# ---- Declare library ----

add_library(
    OpenStudioRecorder_lib OBJECT
    source/lib.cpp
)

target_include_directories(
    OpenStudioRecorder_lib ${warning_guard}
    PUBLIC
    "\$<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/source>"
)

target_compile_features(OpenStudioRecorder_lib PUBLIC cxx_std_20)

find_package(fmt REQUIRED)
target_link_libraries(OpenStudioRecorder_lib PRIVATE fmt::fmt)

# ---- Declare executable ----

add_executable(OpenStudioRecorder_exe WIN32
    source/main.cpp
    source/UserInterface.cxx
    # The recorder's glue onto the vendored JPEG XS demux (plain C).
    source/jxs_range_probe.c
    # Vendored verbatim from the OBS plugin (src/mdns/VENDORED.md): the camera
    # discovery parser and the control-API client. The stack's rule is one parser,
    # never a third - the discovery code is the encoder's own, compiled as C++.
    vendor/jxs/jxs_discovery.cpp
    vendor/jxs/jxs_controls.c
    # Vendored verbatim from obs-jxs-camera (vendor/jxs/VENDORED.md), which is where the
    # JPEG XS stream's colour/range parsing lives: the PMT JXS_video_descriptor and the
    # per-frame jxes box. Copied rather than re-implemented - one definition per constant -
    # and pinned by a sha256 test (test/vendored_jxs_hash.cmake).
    vendor/jxs/jxs_ts.c
    vendor/jxs/s302m.c
    vendor/jxs/mdns/bridge/mdns.cpp
    vendor/jxs/mdns/bridge/discovery.cpp)
add_executable(OpenStudioRecorder::exe ALIAS OpenStudioRecorder_exe)

# The vendored JPEG XS units are C11 (jxs_ts.c uses _Atomic for its counters), exactly as they are
# compiled in the plugin repo - which sets C_STANDARD 11 for the same files. Pinning it here means a
# compiler whose default C dialect is older cannot silently reject the vendored copy.
set_target_properties(OpenStudioRecorder_exe PROPERTIES
    C_STANDARD 11 C_STANDARD_REQUIRED ON)

set_property(TARGET OpenStudioRecorder_exe PROPERTY OUTPUT_NAME OpenStudioRecorder)

target_compile_features(OpenStudioRecorder_exe PRIVATE cxx_std_20)

target_link_libraries(OpenStudioRecorder_exe PRIVATE OpenStudioRecorder_lib fmt::fmt fltk fltk_gl fltk_forms fltk_images)

target_include_directories(OpenStudioRecorder_exe PUBLIC ${FLTK_INCLUDE_DIRS})
target_include_directories(OpenStudioRecorder_exe PUBLIC ${GSTREAMER_INCLUDE_DIRS})
target_include_directories(OpenStudioRecorder_exe PRIVATE ${NDI_INCLUDE_DIRS})
# vendor/jxs for the C shims; vendor/jxs/mdns so the vendored TUs' "bridge/..."
# includes resolve exactly as they do in the plugin repo.
target_include_directories(OpenStudioRecorder_exe PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/vendor/jxs
    ${CMAKE_CURRENT_SOURCE_DIR}/vendor/jxs/mdns)

IF (WIN32)
    target_link_libraries(OpenStudioRecorder_exe PRIVATE ${GSTREAMER_LIBRARIES})
    target_link_libraries(OpenStudioRecorder_exe PRIVATE Threads::Threads)
ELSE()
    target_link_libraries(OpenStudioRecorder_exe PRIVATE
    PkgConfig::gstreamer
    PkgConfig::gstreamer-net
    PkgConfig::gstreamer-sdp
    PkgConfig::gstreamer-rtp
    PkgConfig::gstreamer-app
    PkgConfig::gstreamer-video
    PkgConfig::gio)
ENDIF()

# ---- Install rules ----

if(NOT CMAKE_SKIP_INSTALL_RULES)
  include(cmake/install-rules.cmake)
endif()

# ---- Developer mode ----

if(NOT OpenStudioRecorder_DEVELOPER_MODE)
  return()
elseif(NOT PROJECT_IS_TOP_LEVEL)
  message(
      AUTHOR_WARNING
      "Developer mode is intended for developers of OpenStudioRecorder"
  )
endif()

include(cmake/dev-mode.cmake)
