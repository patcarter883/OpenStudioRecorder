#pragma once

#include <gst/gst.h>
#include <FL/Fl.H>
#include <thread>
#include <vector>
#include <future>
#include <atomic>
#include <mutex>
#include <cstdint>
#include <string>
#include "UserInterface.h"

// Forward declaration of the UI class at global scope so it can be
// referenced as a pointer inside the osr namespace even if the full
// definition is not available here.
class UserInterface;

namespace osr
{

enum class Codec
{
  DNxHR,  // avenc_dnxhd -> mxfmux/.mxf
  ProRes  // avenc_prores_ks -> matroskamux/.mkv
};

enum class Quality
{
  Low,
  Medium,
  High
};

// What the recorder has to do about a camera's quantisation range. Capture units differ: some pass
// the source's range through unchanged, others convert it in hardware. Converting twice compresses
// the picture, so this is decided per camera - never globally.
enum class RangeMode
{
  Passthrough,  // already the range the chain wants; add no element at all
  FullToLimited,  // the unit passed a full-range source through; convert it here
  LimitedToFull  // the unit passed a limited-range source through; convert it here
};

struct PipelineElements
{
  GstElement* pipeline;
  GstElement* source;
  GstElement* convert;
  GstElement* resample;
  GstElement* sink;
};

// How a JXS camera reaches disk. The default decodes and encodes like every other
// camera (the edit suite cannot read JPEG XS); the alternatives record the camera's
// transport stream as it arrives, or transcode it losslessly into Matroska where
// stock ffmpeg can read it. Per camera, because it describes the camera.
enum class JxsRecordMode
{
  Encode,       // decode + the operator's codec and muxer (the default path)
  Passthrough,  // the camera's own MPEG-TS, tapped before tsdemux
  ToMkv         // decode -> FFV1 in Matroska, lossless and portable
};

// A JPEG XS camera on the LAN (the phone node, the Pi node). Deliberately NOT a
// GstDevice: GstDeviceMonitor cannot produce one, and synthesising one would lie to
// every consumer of the device lists. Cameras live in their own list and occupy the
// source slots AFTER the device cameras, so a slot index still identifies a camera
// for producer names, threads and the sidecar.
struct JxsCamera
{
  std::string host;       // the address its stream arrives from
  int port = 5001;        // its stream port
  std::string name;       // operator label (the TXT "name" when discovery found it)
  std::string api_port;   // its HTTP API port, when known
  bool audio = true;      // does this camera carry the ST 302M audio elementary stream
  JxsRecordMode mode = JxsRecordMode::Encode;
};

struct Config
{
  // std::vector<GstDevice*> sources;
  std::vector<GstDevice*> video_sources;
  std::vector<GstDevice*> audio_sources;
  std::vector<std::string> filenames;  // Maps to sources: filenames[i] is the
                                       // output file for sources[i]

  // Per-camera override for the range conversion. An entry here wins; leaving the vector empty (or
  // shorter than the camera list) means "decide from the capture device" - see
  // range_mode_for_camera() in main.cpp.
  // Network cameras, in their own list - see JxsCamera. Source slot numbering is
  // device cameras first, then these.
  std::vector<JxsCamera> jxs_cameras;

  std::vector<RangeMode> range_modes;
  Codec codec = Codec::DNxHR;
  Quality quality = Quality::High;

  int v_width = 1920;
  int v_height = 1080;
  int v_framerate = 30;

  int a_sample_rate = 48000;
  int a_channels = 2;

  int preview_cols = 2;

  GstClockTime base_safety_margin = 5 * GST_MSECOND;
  GstClockTime capture_latency = 40 * GST_MSECOND;
  GstClockTime preview_latency = 230 * GST_MSECOND;
  GstClockTime encode_latency = 1000 * GST_MSECOND;

  GstClockTime max_buffer_time = 1000 * GST_MSECOND;

  // Restart policy for a camera that dies mid-take.
  int restart_max_attempts = 6;               // consecutive failures before giving up
  GstClockTime restart_backoff_min = 1 * GST_SECOND;
  GstClockTime restart_backoff_max = 30 * GST_SECOND;
};

// One file of a camera's take. A camera that dies and comes back writes a NEW segment rather
// than reopening the file it died on (a muxer that latched an error refuses all further work,
// including the EOS that writes the footer/index). Its container timestamps start fresh, so the
// wall-clock instant each segment really began is recorded here instead of being faked into the
// container.
struct SegmentRecord
{
  int index = 1;
  std::string file;
  std::string utc_start;
  std::string reason;   // empty for the first segment of a take
  bool closed = false;  // did the muxer manage to write its footer/index?
};

// Per-camera recording state, owned by that camera's own thread. The UI only reads the atomics.
//
// Cameras are isolated from each other: each has its own capture pipeline (producer side,
// feeding the shared preview pipeline over intersink/intersrc by producer name) and its own
// encode pipeline (consumer side: one encoder chain, one muxer, one file). A camera that dies
// therefore takes down nothing but itself and can be rebuilt in place while every other camera
// keeps recording.
struct SourceState
{
  std::atomic_bool recording{false};  // this camera is currently writing a segment
  std::atomic_bool gave_up{false};    // restart budget exhausted; no longer recording
  std::atomic<int>  segment{1};       // 1 == the first file of the take
  std::atomic<int>  restarts{0};      // successful restarts this session
};

struct App
{
  UserInterface* ui;

  GstDeviceMonitor* audio_monitor;
  GstDeviceMonitor* video_monitor;

  GstDevice* selected_video_device = nullptr;
  GstDevice* selected_audio_device = nullptr;

  std::atomic_bool run = true;              // whole-application run flag
  std::atomic_bool recording = false;       // a take is open and cameras should keep running
  std::atomic_bool stop_requested = false;  // operator asked for a clean stop of everything

  // Preview is shared: every camera's producer feeds it by name, so a camera restart is picked
  // up by the preview automatically and the preview is never touched by a restart.
  GstElement* preview_pipeline = nullptr;
  std::atomic_bool preview_running = false;
  std::atomic_bool preview_eos = false;
  std::future<void> preview_thread;

  // A stop waits for every camera to finalise its file, so it runs off the UI thread.
  std::future<void> shutdown_thread;

  // One thread per camera owns that camera's two pipelines for their whole lifetime, so no two
  // threads ever touch the same pipeline pointer.
  std::vector<std::future<void>> camera_threads;

  GstClock* global_sync_clock = nullptr;

  static constexpr size_t kMaxSources = 16;
  SourceState source[kMaxSources];

  // Per-source capture accounting (fps watchdog + recording sidecar).
  // Bandwidth starvation on USB capture shows up as dropped frames that
  // in-file timestamps cannot reveal, so the app counts frames itself and
  // writes session facts (UTC start, per-clip frame counts, drop estimates,
  // degraded flag) to <clip>.json next to each recording.
  std::atomic<uint64_t> source_frame_counts[kMaxSources] = {};
  std::atomic<int64_t> source_drop_estimates[kMaxSources] = {};

  // Encoder-side accounting: frames leaving each camera's encoder = its file pipeline is really
  // consuming them. The preview's per-source health indicator reads this; a counter that stops
  // moving while a take is open is the failure signal (flags alone must never show green).
  std::atomic<uint64_t> source_encode_counts[kMaxSources] = {};

  // Session lifecycle. session_active = camera threads are running (a take or a preview-only
  // session). detach_requested = the stop must NOT EOS the producer pipelines: used by the
  // preview -> record transition so the shared preview survives and the take's new producers
  // re-register under the same names.
  std::atomic_bool session_active = false;
  std::atomic_bool detach_requested = false;

  std::string session_id;
  std::string session_utc_start;
  GstClockTime session_base_time = GST_CLOCK_TIME_NONE;

  // What happened to each camera: its file segments, and any error/restart events. Guarded by a
  // mutex because the UI thread reads it while camera threads write it.
  std::mutex notes_mutex;
  std::vector<SegmentRecord> source_segments[kMaxSources];
  std::vector<std::string> source_notes[kMaxSources];

  std::atomic_bool watchdog_should_run = false;
  std::future<void> watchdog_thread;
};
}
