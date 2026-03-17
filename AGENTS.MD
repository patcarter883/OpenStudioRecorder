# OpenStudioRecorder - AI Agent Guide

## Project Overview

**OpenStudioRecorder** is a C++20 desktop application for multi-source video/audio recording using GStreamer pipelines with an FLTK GUI.

## Architecture

### Core Components
| Component | Location | Purpose |
|-----------|----------|---------|
| Main executable | `source/main.cpp` | GStreamer pipeline management, UI callbacks |
| UI definition | `source/UserInterface.fl` | FLTK fluid designer file (generates `.h`/`.cxx`) |
| Library stub | `source/lib.cpp`, `source/lib.hpp` | Minimal library separation |
| Build system | `CMakeLists.txt`, `cmake/` | Modern CMake with presets |

### Pipeline Architecture
Three GStreamer pipelines share data via `intersink`/`intersrc` elements:
1. **Capture Pipeline** - Acquires video/audio from devices
2. **Preview Pipeline** - Displays composited multi-source preview
3. **Encode Pipeline** - Writes encoded output files

### Threading Model
- Pipeline message loops run via `std::async`/`std::future` in `osr::app.threads`
- UI updates from worker threads use `Fl::lock()`/`Fl::unlock()` + `Fl::awake()`
- Global state in `osr` namespace: `osr::config`, `osr::app`

## Build & Development

### Prerequisites
- CMake 3.14+ (latest recommended for presets)
- vcpkg with `VCPKG_ROOT` environment variable
- GStreamer (via vcpkg or system packages)

### Quick Start
```sh
# Create CMakeUserPresets.json (see HACKING.md for template)
cmake --preset=dev        # Configure
cmake --build --preset=dev # Build
ctest --preset=dev         # Test
```

### Developer Targets
```sh
cmake --build --preset=dev -t format-check  # Check clang-format
cmake --build --preset=dev -t format-fix    # Auto-fix formatting
cmake --build --preset=dev -t run-exe       # Run executable
```

## Code Conventions

### Formatting
- **80-column limit**, 2-space indentation, no tabs
- **Include order**: Standard library (`<...>`) first, then local (`"..."`)
- See `.clang-format` for full configuration

### Naming
- `snake_case` for functions/variables/methods
- `CamelCase` for classes/types/enums
- `kConstant` style not used

### Example Pattern
```cpp
namespace osr {

enum class Codec { Cineform, DNxHR, ProRes };

struct Config {
  std::vector<GstDevice*> video_sources;
  Codec codec = Codec::Cineform;
};

static std::string make_video_caps_string() {
  return fmt::format("video/x-raw, format=YUY2, width={}", config.v_width);
}

}  // namespace osr
```

## Key Dependencies

| Dependency | Purpose | Notes |
|------------|---------|-------|
| `fmt` | String formatting | `fmt::format()` throughout |
| `fltk` | GUI framework | UI generated via `.fl` fluid files |
| `gstreamer-1.0` | Multimedia | Platform-specific find module |
| `catch2` | Testing | Via "test" feature in vcpkg.json |

## Common Patterns

### Thread-Safe UI Updates
```cpp
void logAppend(std::string logMessage) {
  auto* logMessageCpy = new std::string;
  *logMessageCpy = _strdup(logMessage.c_str());
  Fl::awake(logAppend_cb, logMessageCpy);  // Cross-thread callback
}
```

### GStreamer Pipeline Error Handling
```cpp
GError* error = NULL;
auto pipeline = gst_parse_launch(pipelineStr.c_str(), &error);
if (error) {
  logAppend(fmt::format("Error: {}\n", error->message));
  g_clear_error(&error);
}
```

### FLTK Menu Callbacks (enum via reinterpret_cast)
```cpp
void cb_codec_select(Fl_Menu_*, void* v) {
  osr::config.codec = static_cast<osr::Codec>(reinterpret_cast<std::intptr_t>(v));
}