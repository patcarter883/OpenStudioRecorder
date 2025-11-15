# OpenStudioRecorder AI Coding Agent Instructions

## Project Overview
OpenStudioRecorder is a C++20 media capture application combining **GStreamer** (multimedia framework) with **FLTK** (cross-platform GUI) and **NDI** (network video streaming). The core is a library (`OpenStudioRecorder_lib`) compiled as an object library, with a GUI executable using GStreamer pipelines to manage video sources.

## Architecture & Key Components

### Library Structure
- **`source/lib.hpp/.cpp`** – Minimal core library; the "business logic" container (currently only initializes project name via fmt formatting)
- **`source/main.cpp`** – Heavy lifting: GStreamer device monitoring, pipeline management, UI integration (355+ lines)
- **`source/UserInterface.fl/.cxx/.h`** – FLTK UI definition (auto-generated from `.fl` file)

### Critical Patterns

#### 1. **GStreamer Pipeline Management - Three-Pipeline Architecture**
- Uses `GstDeviceMonitor` to enumerate video sources; queries devices with `gst_device_monitor_get_devices()`
- **Three-tier pipeline architecture** for efficient synchronized capture and flexible output:
  1. **Capture pipelines** (one per source): Timestamp synchronization → video conversion → IPC shmsink for multicast to preview & encode
  2. **Preview pipeline** (singleton): Receives video from capture via shmsrc → compositor for tiling → display
  3. **Encode pipelines** (one per source): Capture video via shmsrc → codec-specific encoder → muxer → filesink
- **Key architectural benefits**:
  - Timestamping ensures all sources remain synchronized in preview and recorded files
  - IPC/shmsink/shmsrc provides zero-copy video sharing between pipelines
  - Capture pipeline handles format conversion once; preview and encode both consume from same source
  - Codec selection (Cineform/DNxHR/ProRes) isolated to encode stage; quality/bitrate applied per-source
- **Key data structures** in `main.cpp`'s `osr` namespace:
  - `Codec` enum – Cineform (avenc_cfhd), DNxHR (avenc_dnxhd), ProRes (avenc_prores_ks)
  - `Quality` enum – Low (50 Mbps), Medium (100 Mbps), High (150 Mbps)
  - `Config` – holds sources, filenames, codec selection, quality setting
  - `App` – manages capture_pipelines, encode_pipelines, preview_pipeline, ipc_socket_names, plus state flags (is_previewing, is_recording)
- Uses atomic flags (`is_previewing`, `is_recording`, `pipeline_error`) for thread-safe state

#### Key Pipeline Functions:
- **`build_capture_pipeline(source, source_idx)`** – Creates timestamping + format conversion + IPC shmsink for one source
- **`build_encode_pipeline(source_idx, filename)`** – Creates encoder + muxer + filesink for one source; pulls from IPC socket
- **`build_preview_pipeline()`** – Creates all capture pipelines, then assembles compositor from their IPC outputs
- **`run_preview()`** – Starts capture + preview pipelines; independent of recording
- **`run_recording()`** – Starts capture (if needed) + encode pipelines; can run simultaneously with preview
- **`stop()`** – Halts all pipelines (preview, encode, capture)

#### 2. **Threading & Async Operations**
- Stores futures in `App.threads` vector for device discovery and pipeline operations
- Uses `std::thread` and `std::future` for blocking GStreamer operations
- **Critical**: FLTK threading requires `Fl::lock()`/`Fl::unlock()` around UI mutations + `Fl::awake()` to signal redraw
  - Example: `findDevices()` populates dropdown then notifies UI safely

#### 3. **Namespace Organization**
- All core logic in **`osr` namespace** – keeps GStreamer/Catch2 symbols isolated
- Callbacks and thread functions are static or global (C linkage expectations for FLTK/GStreamer)
- Use `osr::log()` for stdout output

### External Dependencies & Platform Differences
- **GStreamer**: Conditional CMake discovery
  - Windows: `find_package(GStreamer)` → precompiled binaries
  - Linux/macOS: PkgConfig modules (gstreamer, gstreamer-sdp, gstreamer-rtp, gstreamer-app, gstreamer-video)
- **FLTK**: Cross-platform, config required via CMAKE_INCLUDE_DIRS
- **NDI (optional)**: Only Windows primary development; `FindNDI.cmake` custom module
- **fmt library**: Used for string formatting (linked to both lib and exe)
- **Catch2**: Test framework (3.7.0+, vcpkg feature-gated)

## Build & Development Workflow

### CMake Presets & Configuration
- **Developer Mode**: Set `OpenStudioRecorder_DEVELOPER_MODE=ON` to unlock tests, linters, docs
- **OS-specific presets**: `ci-linux`, `ci-darwin`, `ci-win64` define compiler flags and generators
  - MSVC flags: `/permissive-`, `/volatile:iso`, C++20 standards compliance options
  - GCC/Clang: `-fstack-protector-strong`, `-Werror=float-equal`, and extensive hardening flags
- **vcpkg integration**: `CMAKE_TOOLCHAIN_FILE=${VCPKG_ROOT}/scripts/buildsystems/vcpkg.cmake`
  - Windows preset: `vcpkg-win64-static-md` triple for static linking

### Build Commands
```sh
# Release build (multi-config, e.g., Visual Studio)
cmake -S . -B build
cmake --build build --config Release

# Install
cmake --install build --config Release

# Debug with presets (requires CMakeUserPresets.json)
cmake --preset dev  # Inherits dev-mode, vcpkg, OS-specific preset
cmake --build --preset dev
cmake --test --preset dev
```

### Testing
- **Test discovery**: Catch2 with `catch_discover_tests()`; runs from `test/source/OpenStudioRecorder_test.cpp`
- **Run tests**: `ctest --preset dev` (requires `CMakeUserPresets.json`)
- **Current coverage**: Only `library::library()` initialization tested; GStreamer/FLTK integration lacks test coverage

### Developer Targets (in dev mode)
- `run-exe` – Execute the binary directly
- `format-check`/`format-fix` – Code style enforcement
- `spell-check`/`spell-fix` – Documentation spelling
- `docs` – Doxygen + m.css (if `BUILD_MCSS_DOCS=ON`)
- `coverage` – Code coverage (if `ENABLE_COVERAGE=ON`)
- Linters: `cppcheck`, `clang-tidy` via CMake integration

## Code Conventions & Best Practices

### Naming & Style
- Use **CamelCase** for classes/structs (`PipelineElements`, `GstElement`), **snake_case** for functions (`find_devices`)
- Typedef GStreamer types directly: `GstElement*`, `GstBus*` (no custom wrappers initially)
- Prefix callback functions with `cb`: `add_source_cb()`, `refresh_sources_cb()`

### Memory Management
- GStreamer objects: Manual `gst_object_ref()`/`gst_object_unref()` for lifecycle
- FLTK widgets: Typically heap-allocated; App owns UI pointer
- Strings: Use `std::string` or `fmt::format()` for formatted output

### Error Handling
- **Atomic flags** for error signaling: `App.pipeline_error = true` signals abnormal state
- **GStreamer return checks**: Validate `GstElement*` pointers post-creation; check `gst_device_monitor_start()` success
- **No exceptions**: Project uses `-fno-exceptions` implicitly; use return codes or atomic state flags

### Include Organization
1. Standard library (`<iostream>`, `<string>`, `<vector>`, `<thread>`, `<future>`, `<chrono>`, `<atomic>`)
2. GStreamer (`<gst/gst.h>`)
3. FLTK (`<FL/Fl.H>`, `<FL/Fl_*>`)
4. Project headers (`lib.hpp`, `UserInterface.cxx`)
5. Utility (`<fmt/core.h>`, `<algorithm>`)

## File Organization

```
source/
├── lib.hpp        – Core library interface (minimal; expand here for business logic)
├── lib.cpp        – Library implementation
├── main.cpp       – GStreamer pipelines, FLTK integration, threading orchestration
├── UserInterface.fl – FLTK UI source (generates UserInterface.h/.cxx)
├── UserInterface.h
└── UserInterface.cxx
test/
└── source/OpenStudioRecorder_test.cpp – Catch2 tests (currently trivial)
cmake/
├── FindNDI.cmake – Custom NDI discovery
├── prelude.cmake – Project initialization
├── variables.cmake – Build flags & settings
└── lint-targets.cmake – linter integration
```

## When Adding Features

### New GStreamer Pipeline Elements
- Add to `PipelineElements` struct (main.cpp)
- Update `osr::App` state tracking if stateful (e.g., `is_playing`)
- Thread safety: Use mutex or atomic for shared state; wrap FLTK calls in `Fl::lock()`

### UI Callbacks
- Name with `_cb` suffix; signature: `void callback_name(Fl_Widget*, void*)`
- Access global `osr::app` to mutate state; use `Fl::lock()` before UI updates
- Queue async work via `std::thread` or `std::future` to avoid blocking UI

### Tests
- Use Catch2 macros (`TEST_CASE`, `REQUIRE`, `SECTION`)
- Test library logic first (GStreamer integration currently untested; future target)
- Link test executable to `OpenStudioRecorder_lib` + `Catch2::Catch2WithMain`

## Platform-Specific Notes

- **Windows**: MSVC flags enforce C++ standard compliance; GStreamer via precompiled binaries (vcpkg)
- **Linux/macOS**: Use system GStreamer; PkgConfig for discovery
- **GUI Threading**: FLTK requires `Fl::lock()` for all UI mutations from background threads

## References
- GStreamer docs: https://gstreamer.freedesktop.org/documentation/
- FLTK docs: https://www.fltk.org/doc-1.4/
- CMake Presets: https://cmake.org/cmake/help/latest/manual/cmake-presets.7.html
- vcpkg: https://vcpkg.io
