cmake_minimum_required(VERSION 3.14)
list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
include(cmake/prelude.cmake)

project(
    OpenStudioRecorder
    VERSION 0.1.0
    DESCRIPTION "Short description"
    HOMEPAGE_URL "https://example.com/"
    LANGUAGES CXX
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
    pkg_search_module(gstreamer-sdp REQUIRED IMPORTED_TARGET gstreamer-sdp-1.0>=1.4)
    pkg_search_module(gstreamer-rtp REQUIRED IMPORTED_TARGET gstreamer-rtp-1.0>=1.4)
    pkg_search_module(gstreamer-app REQUIRED IMPORTED_TARGET gstreamer-app-1.0>=1.4)
    pkg_search_module(gstreamer-video REQUIRED IMPORTED_TARGET gstreamer-video-1.0>=1.4)
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

add_executable(OpenStudioRecorder_exe WIN32 source/main.cpp)
add_executable(OpenStudioRecorder::exe ALIAS OpenStudioRecorder_exe)

set_property(TARGET OpenStudioRecorder_exe PROPERTY OUTPUT_NAME OpenStudioRecorder)

target_compile_features(OpenStudioRecorder_exe PRIVATE cxx_std_20)

target_link_libraries(OpenStudioRecorder_exe PRIVATE OpenStudioRecorder_lib fmt::fmt fltk fltk_gl fltk_forms fltk_images)

target_include_directories(OpenStudioRecorder_exe PUBLIC ${FLTK_INCLUDE_DIRS})
target_include_directories(OpenStudioRecorder_exe PUBLIC ${GSTREAMER_INCLUDE_DIRS})
target_include_directories(OpenStudioRecorder_exe PRIVATE ${NDI_INCLUDE_DIRS})

IF (WIN32)
    target_link_libraries(OpenStudioRecorder_exe PRIVATE ${GSTREAMER_LIBRARIES})
    target_link_libraries(OpenStudioRecorder_exe PRIVATE Threads::Threads)
ELSE()
    target_link_libraries(OpenStudioRecorder_exe PRIVATE
    PkgConfig::gstreamer
    PkgConfig::gstreamer-sdp
    PkgConfig::gstreamer-rtp
    PkgConfig::gstreamer-app
    PkgConfig::gstreamer-video)
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
