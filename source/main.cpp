#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>

#include <pthread.h>
#include <chrono>
#include <cstdint>
#include <fstream>
#include <iostream>
#include <set>
#include <string>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <climits>
#include <unistd.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <linux/videodev2.h>  // VIDIOC_QUERYCAP: a UVC unit's metadata node is not a camera
#endif

#include "main.h"

#include <gst/net/gstnet.h>
#include <gst/video/video.h>  // GstVideoInfo, for the match tap's decoded YUY2 geometry; and
                              // GstVideoColorimetry, because a device camera's colour is read, not written
#include <gio/gio.h>  // GSocket: injected into udpsrc for the IP_MULTICAST_ALL fix

#include <FL/Fl.H>
#include <fmt/core.h>

#include "lib.hpp"

#include <FL/Fl_Box.H>
#include <FL/Fl_Check_Button.H>
#include <FL/Fl_Slider.H>
#include <FL/Fl_Value_Slider.H>
#include <FL/Fl_Window.H>

#include "jxs_discovery.h"
#include "range_declaration.h"  // ColorRange + the unit/encoded mapping (shared with the test)
// The recorder's glue onto the VENDORED JPEG XS demux (vendor/jxs/jxs_ts.c). Plain C, and its
// header carries its own extern "C" guard, so it needs no wrapper here - like jxs_controls.h, which
// self-guards too.
#include "jxs_range_probe.h"
// The controls header (control client + image-match helper) is pulled in by main.h. It is
// self-guarding - its whole body sits inside its include guard and, for C++, inside extern "C" -
// so it needs neither a wrapper nor a particular ordering; it is included once, in one place.

// The camera model (defined further down, next to the pipeline builders): device
// cameras occupy the low source slots and JPEG XS network cameras the ones after them.
// Declared here because the watchdog, the source list, the sidecar and the grid
// arithmetic all run earlier in this file.
static size_t camera_count();
static bool is_jxs(size_t i);
static void reset_jxs_declared_ranges();
static const osr::JxsCamera& jxs_of(size_t i);
static std::string camera_label(size_t i);
static bool source_audio(size_t i);
static const char* record_mode_name(size_t i);

namespace osr
{

Config config;
App app;

static void log(std::string message)
{
  std::cout << message << std::endl;
}

static const char* get_codec_element_name(Codec codec)
{
  switch (codec) {
    case Codec::DNxHR:
      return "avenc_dnxhd";
    case Codec::ProRes:
      return "avenc_prores_ks";
    default:
      return "avenc_dnxhd";
  }
}

/* Directory of the running executable. Used to find the patched GStreamer plugins
 * that are shipped next to the application (see configure_gst_plugin_path()). */
static std::string get_executable_dir()
{
#ifdef _WIN32
  wchar_t buffer[MAX_PATH];
  const DWORD length = GetModuleFileNameW(nullptr, buffer, MAX_PATH);
  if (length == 0 || length == MAX_PATH) return {};

  gchar* utf8 = g_utf16_to_utf8(reinterpret_cast<const gunichar2*>(buffer), -1,
                                nullptr, nullptr, nullptr);
  if (utf8 == nullptr) return {};

  const std::string path(utf8);
  g_free(utf8);
  const auto cut = path.find_last_of("\\/");
  return cut == std::string::npos ? std::string{} : path.substr(0, cut);
#else
  char buffer[PATH_MAX];
  const ssize_t length = readlink("/proc/self/exe", buffer, sizeof(buffer) - 1);
  if (length <= 0) return {};

  buffer[length] = '\0';
  const std::string path(buffer);
  const auto cut = path.find_last_of('/');
  return cut == std::string::npos ? std::string{} : path.substr(0, cut);
#endif
}

/* GStreamer catalogues plugins from GST_PLUGIN_PATH before its own system plugin
 * directory, and a plugin found there with the same name replaces the installed one
 * (verified on 1.28.6, including with a warm registry cache). We ship the patched
 * mxfmux - the stock one writes timecode tracks that always start at 00:00:00:00 -
 * so point GStreamer at our copy before it initialises.
 *
 * If the copy is missing, or was built for an older core than the installed runtime,
 * GStreamer refuses to load it ("has incompatible version ... not loading") and the
 * stock plugin is used instead; verify_mxfmux_plugin() reports that at start-up.
 * OSR_GST_PLUGIN_DIR overrides the location (field diagnostics and CI). */
static std::string g_plugin_dir;

static void configure_gst_plugin_path()
{
  const gchar* override_dir = g_getenv("OSR_GST_PLUGIN_DIR");
  if (override_dir != nullptr && *override_dir != '\0') {
    g_plugin_dir = override_dir;
  } else {
    const std::string exe_dir = get_executable_dir();
    if (exe_dir.empty()) {
      log("gst: cannot locate the application directory; not overriding the plugin path");
      return;
    }
    g_plugin_dir = exe_dir + G_DIR_SEPARATOR_S + "gst-plugins";
  }

  std::string value = g_plugin_dir;
  const gchar* existing = g_getenv("GST_PLUGIN_PATH");
  if (existing != nullptr && *existing != '\0') {
    value += G_SEARCHPATH_SEPARATOR_S + std::string(existing);
  }
  g_setenv("GST_PLUGIN_PATH", value.c_str(), TRUE);
  log(fmt::format("gst: plugin path = {}", value));
}

/* Report which plugin provides mxfmux, so a silent fall-back to the stock muxer -
 * and the loss of embedded timecode that comes with it - cannot go unnoticed. */
static void verify_mxfmux_plugin()
{
  GstElementFactory* factory = gst_element_factory_find("mxfmux");
  if (factory == nullptr) {
    log("gst: WARNING - mxfmux is unavailable; MXF recording will fail");
    return;
  }

  /* borrowed reference, do not unref */
  GstPlugin* plugin = gst_plugin_feature_get_plugin(GST_PLUGIN_FEATURE(factory));
  const gchar* filename = plugin != nullptr ? gst_plugin_get_filename(plugin) : nullptr;
  const gchar* version = plugin != nullptr ? gst_plugin_get_version(plugin) : nullptr;

  log(fmt::format("gst: mxfmux from '{}' (plugin {}, runtime {})",
      filename != nullptr ? filename : "unknown",
      version != nullptr ? version : "unknown", gst_version_string()));

  const bool ours = !g_plugin_dir.empty() && filename != nullptr &&
                    std::string(filename).rfind(g_plugin_dir, 0) == 0;
  if (!ours) {
    log("gst: WARNING - the stock mxfmux is active, MXF timecode will start at 00:00:00:00");
  }

  gst_object_unref(factory);
}

static const char* get_muxer_name(Codec codec)
{
  switch (codec) {
    case Codec::DNxHR:
      return "mxfmux";  // verified clean with DNxHR+audio on gst 1.28.6; the 2025
                        // audio-timestamp failure no longer reproduces. NB: do NOT
                        // switch this to avmux_mxf - its gst pad templates accept
                        // only video/mpeg, so it cannot mux DNxHD at all.
    case Codec::ProRes:
      // matroskamux advertises video/x-prores, and it carries the full colour set - range
      // included - where a QuickTime container cannot carry range at all. Measured on a
      // live camera feed, 2026-10.
      return "matroskamux";
    default:
      return "mxfmux";
  }
}

static const char* get_filename_extension(Codec codec)
{
  switch (codec) {
    case Codec::ProRes:
      return ".mkv";
    case Codec::DNxHR:
      return ".mxf";
    default:
      return ".mxf";
  }
}

// Properties for the muxer element that make the output survive a crash or
// power loss. Neither container this app writes needs any: MXF essence is
// self-describing and appended as written, and Matroska is written as a
// stream with no index that only exists at EOF, so a kill costs the tail of
// the file rather than the whole take. (An unfragmented QuickTime file would
// need fragment-duration here - this app no longer writes MOV.)
static const char* get_quality_name(Codec codec)
{
  switch (codec) {
    case Codec::DNxHR:
    case Codec::ProRes:
      return "profile";
    default:
      return "profile";
  }
}

static int get_setting_for_quality(Quality quality, Codec codec)
{
  switch (codec) {
    case Codec::DNxHR:
      // 8-bit profiles only: the captures are 8-bit (YUY2), so dnxhr_hqx (10-bit) adds no
      // information and does not even negotiate without a pinned 10-bit capsfilter.
      switch (quality) {
        case Quality::Low:
          return 1;  // Dnxhd-profile dnxhr_lb
        case Quality::Medium:
          return 2;  // Dnxhd-profile dnxhr_sq
        case Quality::High:
          return 3;  // Dnxhd-profile dnxhr_hq
        default:
          return 3;
      }
    case Codec::ProRes:
      switch (quality) {
        case Quality::Low:
          return 1;  // Prores-encoder-profile lt
        case Quality::Medium:
          return 2;  // Prores-encoder-profile standard
        case Quality::High:
          return 3;  // Prores-encoder-profile hq
        default:
          return 2;
      }
    default:
      return 3;  // hq: the quality selector's High maps here, so keep the fallback at it
  }
}

// The depth of the essence the encoder writes, keyed on the very profile this file selects for
// it - not a second opinion about the codec. avenc_dnxhd advertises no depth on its src caps, so
// the muxer cannot read it from the stream; this mapping is the only place that knows it.
// Profiles 1-3 (dnxhr_lb/sq/hq) are 8-bit; 4 (dnxhr_hqx) is 10-bit and is not currently offered.
static int component_depth_for_profile(int profile)
{
  switch (profile) {
    case 1:  // dnxhr_lb
    case 2:  // dnxhr_sq
    case 3:  // dnxhr_hq
      return 8;
    case 4:  // dnxhr_hqx
      return 10;
    default:
      return 8;
  }
}

static std::string get_muxer_properties(Codec codec, Quality quality)
{
  switch (codec) {
    case Codec::DNxHR:
      // mxfmux writes the descriptor's component depth from this property; it has no other
      // source, and every colour item comes from the caps without one.
      return "component-depth=" +
          std::to_string(component_depth_for_profile(get_setting_for_quality(quality, codec)));
    case Codec::ProRes:
      // matroskamux has no fragment-duration, and setting an unknown property on it would
      // leave the failure to a parse warning. MKV needs nothing here: it is written as a
      // stream with no index that only exists at EOF, so a kill mid-recording costs the
      // tail rather than the file.
      return "";
    default:
      return "";
  }
}

static std::string get_ipc_pipeline_name(size_t index)
{
  return fmt::format("video{}", index);
  // return fmt::format("\\\\.\\pipe\\gst.win32.ipc.video{}", index);
}

}  // namespace osr

void logAppend_cb(void* msgPtr)
{
  std::string* msgStr = static_cast<std::string*>(msgPtr);
  osr::app.ui->logDisplay->insert(msgStr->c_str());
  osr::app.ui->logDisplay->insert("\n");
  // Delete through the pointer's own type. Deleting the void* it arrived as is
  // undefined and skips ~std::string, so the string's heap block leaked on every
  // log line.
  delete msgStr;
  return;
}

void logAppend(std::string logMessage)
{
  auto* logMessageCpy = new std::string;
  *logMessageCpy = std::move(logMessage);
  Fl::awake(logAppend_cb, logMessageCpy);
  return;
}


// --- capture accounting: fps watchdog + per-clip recording sidecar ---------

static GstPadProbeReturn count_frame_cb(GstPad*, GstPadProbeInfo*, gpointer user_data)
{
  const auto idx = static_cast<size_t>(GPOINTER_TO_INT(user_data));
  if (idx < osr::app.kMaxSources)
    osr::app.source_frame_counts[idx].fetch_add(1, std::memory_order_relaxed);
  return GST_PAD_PROBE_OK;
}

// Frames leaving a camera's encoder - the file-writing side of its chain. The preview's health
// indicator turns red when this stops advancing while a take is open.
static GstPadProbeReturn count_encode_cb(GstPad*, GstPadProbeInfo*, gpointer user_data)
{
  const auto idx = static_cast<size_t>(GPOINTER_TO_INT(user_data));
  if (idx < osr::app.kMaxSources)
    osr::app.source_encode_counts[idx].fetch_add(1, std::memory_order_relaxed);
  return GST_PAD_PROBE_OK;
}

// --- preview health indicator ----------------------------------------------
// Every preview tile carries a textoverlay ("CAM n OK" / "CAM n FAIL") whose colour is driven by
// the real frame counters, so the indicator cannot show green from a flag alone. While a take is
// open health means "frames are leaving this camera's encoder" (the file side is alive); outside
// a take it means "frames are arriving from this camera's capture" (the source is alive). A
// counter that has not moved for two seconds is the failure signal.
struct HealthTracker
{
  uint64_t capture = 0;
  uint64_t encode = 0;
  GstClockTime capture_moved = 0;
  GstClockTime encode_moved = 0;
};
static HealthTracker g_health[osr::App::kMaxSources];
static bool g_health_timer_started = false;
static const GstClockTime kHealthStale = 2 * GST_SECOND;

static void health_tick_cb(void*)
{
  if (!osr::app.preview_running.load()) {
    g_health_timer_started = false;  // preview gone: stop the timer, start_preview() re-arms
    return;
  }

  const GstClockTime now = gst_util_get_timestamp();
  const bool recording = osr::app.recording.load();

  for (size_t i = 0; i < camera_count() && i < osr::App::kMaxSources; ++i) {
    const uint64_t capture = osr::app.source_frame_counts[i].load(std::memory_order_relaxed);
    const uint64_t encode = osr::app.source_encode_counts[i].load(std::memory_order_relaxed);
    if (capture != g_health[i].capture) {
      g_health[i].capture = capture;
      g_health[i].capture_moved = now;
    }
    if (encode != g_health[i].encode) {
      g_health[i].encode = encode;
      g_health[i].encode_moved = now;
    }

    bool healthy;
    if (osr::app.source[i].gave_up.load())
      healthy = false;
    else if (recording)
      healthy = osr::app.source[i].recording.load()
                && g_health[i].encode_moved != 0
                && now - g_health[i].encode_moved < kHealthStale;
    else
      healthy = g_health[i].capture_moved != 0
                && now - g_health[i].capture_moved < kHealthStale;

    if (osr::app.preview_pipeline) {
      GstElement* overlay = gst_bin_get_by_name(
          GST_BIN(osr::app.preview_pipeline), fmt::format("health{}", i).c_str());
      if (overlay) {
        g_object_set(overlay,
                     "text", fmt::format("CAM {} {}", i, healthy ? "OK" : "FAIL").c_str(),
                     "color", healthy ? 0xFF00FF00u : 0xFFFF0000u,
                     NULL);
        gst_object_unref(overlay);
      }
    }
  }
  Fl::repeat_timeout(0.5, health_tick_cb);
}

static std::string json_escape(const std::string& in)
{
  std::string out;
  out.reserve(in.size() + 8);
  for (const char c : in) {
    switch (c) {
      case '"': out += "\\\""; break;
      case '\\': out += "\\\\"; break;
      case '\n': out += "\\n"; break;
      case '\r': out += "\\r"; break;
      case '\t': out += "\\t"; break;
      default: out += c; break;
    }
  }
  return out;
}

static void frame_rate_watchdog()
{
  uint64_t prev[osr::app.kMaxSources] = {};
  int low_ticks[osr::app.kMaxSources] = {};
  const int fps = osr::config.v_framerate;
  while (osr::app.watchdog_should_run.load()) {
    g_usleep(G_USEC_PER_SEC);
    size_t n = camera_count();
    if (n > osr::app.kMaxSources) n = osr::app.kMaxSources;
    for (size_t i = 0; i < n; ++i) {
      const uint64_t now = osr::app.source_frame_counts[i].load(std::memory_order_relaxed);
      const int64_t delta = static_cast<int64_t>(now - prev[i]);
      prev[i] = now;
      if (fps > 0 && delta < fps * 8 / 10) {
        if (++low_ticks[i] >= 2) {
          osr::app.source_drop_estimates[i].fetch_add(fps - delta);
          logAppend(fmt::format(
              "WARN: source {} delivering {} fps (nominal {}); estimated drops so far {}",
              i, delta, fps, osr::app.source_drop_estimates[i].load()));
        }
      } else {
        low_ticks[i] = 0;
      }
    }
  }
}

// Defined with the encode builder; declared here because the sidecar needs the same naming.
static std::string segment_path(size_t i);

// Write one sidecar per segment file, each carrying the whole story for that camera: every file
// it wrote, when each really began, and why it was restarted. A camera that drops out and comes
// back starts a NEW segment rather than reopening the file it died on, and its container
// timestamps begin fresh - so the wall-clock instant is recorded here instead of being faked into
// the file. A segment with "closed": false never got its footer/index (its muxer latched an error
// and refused the EOS); the footage is intact and repacks losslessly with
// `ffmpeg -i <file> -c copy <fixed>`.
static void write_recording_sidecars(bool final)
{
  gchar* utc_stop = final ? g_date_time_format_iso8601(g_date_time_new_now_utc()) : nullptr;
  for (size_t i = 0; i < osr::config.filenames.size(); ++i) {
    // One label for either kind of camera: a network camera has no GstDevice to ask.
    const std::string device = camera_label(i);

    std::vector<osr::SegmentRecord> segments;
    std::vector<std::string> notes;
    {
      std::lock_guard<std::mutex> lock(osr::app.notes_mutex);
      if (i < osr::App::kMaxSources) {
        segments = osr::app.source_segments[i];
        notes = osr::app.source_notes[i];
      }
    }
    if (segments.empty()) {
      osr::SegmentRecord rec;
      rec.index = 1;
      rec.file = segment_path(i);
      segments.push_back(rec);
    }

    const uint64_t frames = i < osr::App::kMaxSources ? osr::app.source_frame_counts[i].load() : 0;
    const int64_t drops = i < osr::App::kMaxSources ? osr::app.source_drop_estimates[i].load() : 0;
    const int restarts = i < osr::App::kMaxSources ? osr::app.source[i].restarts.load() : 0;
    const bool gave_up = i < osr::App::kMaxSources && osr::app.source[i].gave_up.load();

    for (const auto& seg : segments) {
      std::ofstream out(seg.file + ".json", std::ios::trunc);
      if (!out) {
        logAppend(fmt::format("WARN: cannot write sidecar for {}", seg.file));
        continue;
      }
      out << "{\n"
          << "  \"session_id\": \"" << json_escape(osr::app.session_id) << "\",\n"
          << "  \"clip\": \"" << json_escape(seg.file) << "\",\n"
          << "  \"device\": \"" << json_escape(device) << "\",\n"
          << "  \"camera_index\": " << i << ",\n"
          << "  \"utc_start\": \"" << json_escape(osr::app.session_utc_start) << "\",\n"
          << "  \"session_monotonic_base_ns\": " << osr::app.session_base_time << ",\n"
          << "  \"codec\": \"" << osr::get_codec_element_name(osr::config.codec) << "\",\n"
          << "  \"mode\": \"" << record_mode_name(i) << "\",\n"
          // What happened to the audio: a network camera's stream carries ST 302M
          // (AES3-packed PCM) which is decoded once and written as 24-bit PCM, so there
          // is no audio codec anywhere on the link or in the file.
          << "  \"audio\": "
          << (source_audio(i)
                  ? "{\"codec\":\"pcm-s24le-302m\",\"rate\":48000,\"channels\":2,"
                    "\"mode\":\"decoded-pcm\"}"
                  : "null")
          << ",\n"
          << "  \"width\": " << osr::config.v_width << ",\n"
          << "  \"height\": " << osr::config.v_height << ",\n"
          << "  \"nominal_fps\": " << osr::config.v_framerate << ",\n"
          << "  \"status\": \"" << (final ? "closed" : "recording") << "\",\n"
          << "  \"frames_written\": " << frames << ",\n"
          << "  \"drop_estimate\": " << drops << ",\n"
          << "  \"restarts\": " << restarts << ",\n"
          << "  \"gave_up\": " << (gave_up ? "true" : "false") << ",\n";
      if (utc_stop)
        out << "  \"utc_stop\": \"" << (utc_stop ? utc_stop : "") << "\",\n";
      out << "  \"segments\": [\n";
      for (size_t k = 0; k < segments.size(); ++k) {
        out << "    {\"index\": " << segments[k].index
            << ", \"file\": \"" << json_escape(segments[k].file) << "\""
            << ", \"utc_start\": \"" << json_escape(segments[k].utc_start) << "\""
            << ", \"reason\": \"" << json_escape(segments[k].reason) << "\""
            << ", \"closed\": " << (segments[k].closed ? "true" : "false") << "}"
            << (k + 1 < segments.size() ? "," : "") << "\n";
      }
      out << "  ],\n"
          << "  \"events\": [\n";
      for (size_t k = 0; k < notes.size(); ++k) {
        out << "    \"" << json_escape(notes[k]) << "\""
            << (k + 1 < notes.size() ? "," : "") << "\n";
      }
      out << "  ]\n}\n";
    }
  }
  if (utc_stop) g_free(utc_stop);
}

static void begin_recording_session()
{
  // Quiesce any previous watchdog before clearing per-session state.
  osr::app.watchdog_should_run.store(false);
  if (osr::app.watchdog_thread.valid()) osr::app.watchdog_thread.wait();
  for (size_t i = 0; i < osr::app.kMaxSources; ++i) {
    osr::app.source_frame_counts[i].store(0);
    osr::app.source_drop_estimates[i].store(0);
    osr::app.source_encode_counts[i].store(0);
  }
  gchar* hex = g_strdup_printf("%08x%08x", g_random_int(), g_random_int());
  GDateTime* now = g_date_time_new_now_utc();
  gchar* iso = g_date_time_format_iso8601(now);
  osr::app.session_id = hex;
  osr::app.session_utc_start = iso;
  osr::app.session_base_time = osr::app.global_sync_clock
      ? gst_clock_get_time(osr::app.global_sync_clock) : GST_CLOCK_TIME_NONE;
  g_free(hex);
  g_free(iso);
  g_date_time_unref(now);
  osr::app.watchdog_should_run.store(true);
  osr::app.watchdog_thread = std::async(std::launch::async, frame_rate_watchdog);
}

void cb_video_source_select(Fl_Widget* w, void* data)
{
  auto choice = static_cast<Fl_Choice*>(w);
  auto device = static_cast<GstDevice*>(data);
  osr::app.selected_video_device = device;
}

void cb_audio_source_select(Fl_Widget* w, void* data)
{
  auto choice = static_cast<Fl_Choice*>(w);
  auto device = static_cast<GstDevice*>(data);
  osr::app.selected_audio_device = device;
}

// The two helpers dedupe_devices() and refresh_video_sources() call. They are defined further
// down (next to the other source-element helpers); the declarations belong HERE, above the first
// use - the landed device-colorimetry work moved the calls above them, so its own tip (01de796)
// does not compile on its own. Declaring them here is what the code always meant; it changes no
// function, and nothing of that branch's work is dropped.
static std::string capture_node_key(GstStructure* props);
static bool is_pipewire_device(GstStructure* props);

// The GstDevice objects stay owned by the monitor's list, so this returns pointers into it; the
// caller adds them to the chooser and frees nothing.
// One entry per physical unit. Two providers report the same hardware, so for each key this keeps
// the PREFERRED provider's devices and drops the other provider's copy: every PipeWire node for that
// unit when PipeWire is preferred (one card can expose more than one node), every V4L or ALSA device
// when it is not. Devices from the SAME provider are never collapsed into each other - they are
// distinct things to open even when they share a card.
static std::vector<GstDevice*> dedupe_devices(GList* device_list,
                                             std::string (*key_of)(GstStructure*),
                                             bool prefer_pipewire)
{
  struct Entry
  {
    GstDevice* device;
    std::string key;
    bool pipewire;
  };

  std::vector<Entry> entries;
  for (GList* node = device_list; node != nullptr; node = node->next)
  {
    auto* device = static_cast<GstDevice*>(node->data);
    GstStructure* props = gst_device_get_properties(device);
    if (props == nullptr)
      continue;
    entries.push_back({device, key_of(props), is_pipewire_device(props)});
    gst_structure_free(props);
  }

  std::vector<GstDevice*> kept;
  for (const Entry& entry : entries)
  {
    // Without a key there is nothing to compare against, so the device is always offered.
    bool unit_has_preferred = false;
    if (!entry.key.empty())
      for (const Entry& other : entries)
        if (other.key == entry.key && other.pipewire == prefer_pipewire)
          unit_has_preferred = true;
    if (unit_has_preferred && entry.pipewire != prefer_pipewire)
      continue;
    kept.push_back(entry.device);
  }
  return kept;
}

// Can this node actually capture? Every UVC unit exposes TWO nodes: the capture node, and a METADATA
// node on the next /dev/videoN whose capability is V4L2_CAP_META_CAPTURE and NOT
// V4L2_CAP_VIDEO_CAPTURE. Offered as if it were a camera, the metadata node opens happily and then
// fails at the first poll:
//   capture error from capture_src: Could not read from resource
//   gst_v4l2_object_poll(): poll error 1: Invalid argument (22)
// which reads like a hardware fault. So the list asks the kernel, exactly as the OBS source has
// always done, instead of trusting a name or the caps. When the node cannot be opened to ask (busy,
// or no permission) it is KEPT: "cannot tell" must never hide real hardware.
static bool node_is_metadata_only(const std::string& path)
{
#if defined(_WIN32)
  (void)path;
  return false;
#else
  if (path.empty())
    return false;
  int fd = open(path.c_str(), O_RDWR | O_NONBLOCK);
  if (fd < 0)
    fd = open(path.c_str(), O_RDONLY | O_NONBLOCK);
  if (fd < 0)
    return false;
  struct v4l2_capability cap = {};
  const bool got_caps = ioctl(fd, VIDIOC_QUERYCAP, &cap) == 0;
  close(fd);
  if (!got_caps)
    return false;
  const guint32 caps =
      (cap.capabilities & V4L2_CAP_DEVICE_CAPS) ? cap.device_caps : cap.capabilities;
  return (caps & V4L2_CAP_VIDEO_CAPTURE) == 0;
#endif
}

static bool device_is_metadata_node(GstDevice* device)
{
  GstStructure* props = gst_device_get_properties(device);
  if (props == nullptr)
    return false;
  const std::string path = capture_node_key(props);
  gst_structure_free(props);
  return node_is_metadata_only(path);
}

static void refresh_video_sources()
{
  auto device_list = gst_device_monitor_get_devices(osr::app.video_monitor);
  // Video prefers the V4L device: v4l2src is what this recorder captures with (see
  // video_source_element), so the PipeWire node of the same unit is the duplicate to hide.
  const std::vector<GstDevice*> deduped = dedupe_devices(device_list, capture_node_key, false);
  // Drop the nodes that cannot capture (see node_is_metadata_only): offering a unit's metadata node
  // as a second camera is what makes a healthy unit look broken at its first poll.
  std::vector<GstDevice*> devices;
  for (GstDevice* device : deduped)
    if (!device_is_metadata_node(device))
      devices.push_back(device);

  std::string offered;
  for (GstDevice* device : devices)
  {
    auto* name = gst_device_get_display_name(device);
    offered += fmt::format("{}{}", offered.empty() ? "" : " | ", name != nullptr ? name : "?");
    g_free(name);
  }
  // State what the list holds, so "my card is not in the list" has an answer in the log.
  logAppend(fmt::format("Sources: {} video device(s) offered: {}", devices.size(), offered));

  Fl::lock();
  // Rebuilt from scratch on every refresh. Appending to a populated chooser is how the list grew a
  // second copy of every unit on each Refresh, with the older entries' callback data still pointing
  // at devices from a list the monitor had already replaced.
  osr::app.ui->video_source_select->clear();
  osr::app.selected_video_device = devices.empty() ? nullptr : devices.front();
  for (GstDevice* device : devices)
  {
    auto* device_name = gst_device_get_display_name(device);
    osr::app.ui->video_source_select->add(
        device_name, 0, (Fl_Callback*)cb_video_source_select, device, 0);
    g_free(device_name);
  }
  if (!devices.empty())
    osr::app.ui->video_source_select->value(1);  // FLTK counts items from 1
  Fl::unlock();
  Fl::awake();
  // g_list_free_full(device_list, (GDestroyNotify)gst_object_unref);
}

// Audio devices have no kernel capture node, so the "same unit" key is the ALSA CARD they sit on.
//
// The card has to be read from whichever form the provider uses, because the two providers describe
// the same card differently: a PipeWire node states api.alsa.pcm.card (and api.alsa.path, "hw:4,0"),
// the alsa deviceprovider states alsa.card. Requiring a specific field - or an int - is what left the
// list unfiltered: the ALSA copy carried alsa.card but no pcm-device field, so its key differed from
// the PipeWire node's and nothing was recognised as a duplicate.
//
// Keying on the card alone is deliberate. Within one provider two nodes on a card are different
// things to open (a card can expose a "Pro" and an "Analog Stereo" profile) and are both kept, since
// dedupe only drops the OTHER provider's copy of a unit.
static std::string prop_text(GstStructure* props, const char* field)
{
  const gchar* text = gst_structure_get_string(props, field);
  if (text != nullptr && *text != '\0')
    return text;
  gint number = 0;
  if (gst_structure_get_int(props, field, &number))
    return std::to_string(number);
  return {};
}

static std::string alsa_card_key(GstStructure* props)
{
  std::string card = prop_text(props, "api.alsa.pcm.card");
  if (card.empty())
    card = prop_text(props, "alsa.card");
  if (card.empty())
  {
    // "hw:4,0" (capture) or "front:0" (an onboard card): the card follows the colon.
    const std::string path = prop_text(props, "api.alsa.path");
    const auto colon = path.find(':');
    if (colon == std::string::npos)
      return {};
    const auto comma = path.find(',', colon);
    card = path.substr(colon + 1, comma == std::string::npos ? std::string::npos : comma - colon - 1);
  }
  return card;
}

static void refresh_audio_sources()
{
  auto device_list = gst_device_monitor_get_devices(osr::app.audio_monitor);
  // Audio prefers the PipeWire node: it is named (a serial does not survive a restart), it works
  // while another consumer holds the card, and the ALSA device it sits on is the duplicate.
  const std::vector<GstDevice*> devices = dedupe_devices(device_list, alsa_card_key, true);

  std::string offered;
  for (GstDevice* device : devices)
  {
    auto* name = gst_device_get_display_name(device);
    offered += fmt::format("{}{}", offered.empty() ? "" : " | ", name != nullptr ? name : "?");
    g_free(name);
  }
  logAppend(fmt::format("Sources: {} audio device(s) offered: {}", devices.size(), offered));

  Fl::lock();
  // Rebuilt from scratch on every refresh, for the same reason as the video list: appending to a
  // populated chooser duplicated every unit on each Refresh.
  osr::app.ui->audio_source_select->clear();
  osr::app.selected_audio_device = devices.empty() ? nullptr : devices.front();
  for (GstDevice* device : devices)
  {
    auto* device_name = gst_device_get_display_name(device);
    osr::app.ui->audio_source_select->add(
        device_name, 0, (Fl_Callback*)cb_audio_source_select, device, 0);
    g_free(device_name);
  }
  if (!devices.empty())
    osr::app.ui->audio_source_select->value(1);  // FLTK counts items from 1
  Fl::unlock();
  Fl::awake();
  // g_list_free_full(device_list, (GDestroyNotify)gst_object_unref);
}

static void* findDevices()
{
  GstCaps* caps;

  osr::app.video_monitor = gst_device_monitor_new();
  // caps = gst_caps_new_any();
  gst_device_monitor_add_filter(
      osr::app.video_monitor, "Video/Source", nullptr);
  // gst_caps_unref(caps);

  refresh_video_sources();

  osr::app.audio_monitor = gst_device_monitor_new();
  // caps = gst_caps_new_any();
  gst_device_monitor_add_filter(
      osr::app.audio_monitor, "Audio/Source", nullptr);
  // gst_caps_unref(caps);

  refresh_audio_sources();

  return NULL;
}

static void initUi()
{
  osr::app.ui = new UserInterface;
  osr::app.ui->logDisplay->buffer(new Fl_Text_Buffer);
  // show() takes (int argc, char** argv): 0 is "no arguments to parse". NULL is a
  // pointer constant and does not belong in the int parameter.
  osr::app.ui->show(0, nullptr);
}

void refresh_sources_cb(Fl_Button*, void*)
{
  refresh_video_sources();
  refresh_audio_sources();
}

// A source may legitimately have no audio device: adding a video-only source is by design. Every
// place that touches the audio device must therefore check first - GStreamer's device helpers take
// a real GstDevice, and calling them on a null pointer is a null dereference, not a clean error
// return, so an audio-less source used to take the app down instead of just recording video.
static size_t camera_count()
{
  return osr::config.video_sources.size() + osr::config.jxs_cameras.size();
}

// Device cameras take the low slots, network cameras the ones after them.
static bool is_jxs(size_t i)
{
  return i >= osr::config.video_sources.size() && i < camera_count();
}

static const osr::JxsCamera& jxs_of(size_t i)
{
  return osr::config.jxs_cameras[i - osr::config.video_sources.size()];
}

// The label every list, log line and sidecar shows for a slot.
static std::string camera_label(size_t i)
{
  if (is_jxs(i)) {
    const osr::JxsCamera& cam = jxs_of(i);
    // Multicast: the camera's channel is its GROUP (one shared port); unicast:
    // the classic address:port.
    const std::string addr = cam.transport == osr::JxsTransport::Multicast
                                 ? cam.group
                                 : fmt::format("{}:{}", cam.host, cam.port);
    return cam.name.empty() ? fmt::format("JXS {}", addr)
                            : fmt::format("{} (JXS {})", cam.name, addr);
  }
  if (i < osr::config.video_sources.size() && osr::config.video_sources[i]) {
    gchar* dn = gst_device_get_display_name(osr::config.video_sources[i]);
    std::string out = dn ? dn : "";
    g_free(dn);
    return out;
  }
  return fmt::format("CAM {}", i);
}

// A slot's audio: a device camera has audio when a device is selected; a network
// camera has audio when the operator says its stream carries the ST 302M ES. In the
// ToMkv mode the audio is decoded into the file inside the producer and never crosses
// the IPC hop, so there is nothing for the consumer or the preview mixer to wait for.
// The sidecar's "mode" key: what happened to this camera's picture on the way to disk.
// A device camera is always decoded and encoded; a network camera's own mode decides.
static const char* record_mode_name(size_t i)
{
  if (!is_jxs(i))
    return "encode";
  switch (jxs_of(i).mode) {
    case osr::JxsRecordMode::Passthrough:
      return "jxs_passthrough";
    case osr::JxsRecordMode::ToMkv:
      return "jxs_to_mkv";
    default:
      return "encode";
  }
}

static std::string audio_source_element(GstDevice* device);
static std::string video_source_element(GstDevice* device);
static std::string capture_node_key(GstStructure* props);

// Two providers report the SAME physical unit, so an unfiltered monitor list offers every capture
// unit twice: once as a PipeWire node ("UGREEN 65518 (V4L2)", identity node.name, caps carrying the
// device's colour, shareable with other consumers) and once as the raw V4L2 device the
// v4l2deviceprovider describes (identity device.path, minimal caps, exclusive open). The list keeps
// one entry per unit and prefers the PipeWire node, because that is the one that carries the colour
// and that another consumer (OBS) can hold at the same time. A unit with no PipeWire node - a host
// without PipeWire - keeps its V4L2 entry; nothing is hidden that does not have a replacement.
static bool is_pipewire_device(GstStructure* props);

// The key that says "same physical unit" across providers: the kernel capture node. A PipeWire node
// reports it as api.v4l2.path, the v4l2 provider as device.path.
static std::string capture_node_key(GstStructure* props)
{
  const gchar* path = gst_structure_get_string(props, "api.v4l2.path");
  if (path == nullptr || *path == '\0')
    path = gst_structure_get_string(props, "device.path");
  return path != nullptr ? std::string(path) : std::string{};
}

static bool is_pipewire_device(GstStructure* props)
{
  const gchar* node = gst_structure_get_string(props, "node.name");
  const gchar* serial = gst_structure_get_string(props, "object.serial");
  return (node != nullptr && *node != '\0') || (serial != nullptr && *serial != '\0');
}

static bool source_audio(size_t i)
{
  if (is_jxs(i)) {
    const osr::JxsCamera& cam = jxs_of(i);
    return cam.audio && cam.mode != osr::JxsRecordMode::ToMkv;
  }
  if (!(i < osr::config.audio_sources.size() && osr::config.audio_sources[i] != nullptr))
    return false;
  // A device no known element can open has no audio leg. Returning true here would make the preview
  // and the encode leg wait for a producer that never appears - an audiomixer with no input never
  // prerolls - so the answer has to come from the device, in the one place that decides it.
  return !audio_source_element(osr::config.audio_sources[i]).empty();
}

// (the device-camera arm of source_audio is folded into the definition above)

// Bumped whenever the camera set changes. The preview's tiles are built from that set and the preview
// pipeline is otherwise never rebuilt, so this is what tells start_preview() the tiles are stale.
static int g_camera_set_generation = 0;

static void update_source_list()
{
  // Every camera needs a filename: it is what names its take, and a slot without one cannot record.
  // The vector is kept aligned with the camera set here, at the one place the set changes, so a
  // camera added by either path (device or JXS) can never end up with no name.
  while (osr::config.filenames.size() < camera_count())
    osr::config.filenames.emplace_back(fmt::format("cam{}", osr::config.filenames.size()));
  // The camera set moved, so any preview tiles built from the old one are stale.
  ++g_camera_set_generation;

  Fl::lock();
  osr::app.ui->browser_source_list->clear();
  for (size_t i = 0; i < camera_count(); ++i) {
    // A network camera has no GstDevice and no device audio: its label and its audio
    // are its own. One line per slot, whatever kind of camera it is.
    const std::string video_name = camera_label(i);
    std::string audio_name = "(no audio)";
    if (source_audio(i)) {
      if (is_jxs(i)) {
        audio_name = "302M in stream";
      } else if (i < osr::config.audio_sources.size() && osr::config.audio_sources[i]) {
        gchar* an = gst_device_get_display_name(osr::config.audio_sources[i]);
        audio_name = an ? an : "(no audio)";
        g_free(an);
      }
    }
    auto display_text = fmt::format("{} / {} -> {}",
                                    video_name, audio_name, osr::config.filenames[i]);
    osr::app.ui->browser_source_list->add(display_text.c_str());
  }
  Fl::unlock();
  Fl::awake();
}

// ---------------------------------------------------------------------------
// JPEG XS camera discovery and control (the brief's Runs 2 and 3)
// ---------------------------------------------------------------------------
//
// Both windows are built in code rather than in the .fl, for the same reason the OBS
// plugin builds its properties pane in code: a camera's control set is not known until
// it answers, so the widgets are made from the descriptor at run time. The .fl carries
// only the two buttons that open these.
//
// Discovery and the control client are the OBS plugin's own translation units, vendored
// verbatim (vendor/jxs, see its VENDORED.md) - the stack's rule is one parser, never a
// third one. Nothing here speaks DNS-SD or parses JSON itself.

// The slot's control API base URL, or empty when it is a device camera or its API port
// was never learned (a typed-in address without a scan has none, and that is legal).
static std::string jxs_base_url(size_t i)
{
  if (!is_jxs(i))
    return "";
  const osr::JxsCamera& cam = jxs_of(i);
  if (cam.api_port.empty())
    return "";
  return fmt::format("http://{}:{}", cam.host, cam.api_port);
}

// window lifetime is the window's own: these are small, opened on demand and reused.
struct JxsCamUi
{
  Fl_Window* win = nullptr;
  Fl_Browser* found = nullptr;
  Fl_Input* host = nullptr;
  Fl_Input* api = nullptr;
  Fl_Input* name = nullptr;
  // Multicast: the transport selector plus the group block/index. The group is
  // ONE fixed port for every camera, so there is no per-camera port field here.
  Fl_Choice* transport = nullptr;
  Fl_Input* group_base = nullptr;
  Fl_Input* group_index = nullptr;
  std::atomic_bool scanning{false};
  jxs_discovered_camera_t results[JXS_DISC_MAX_CAMERAS];
  int n_results = 0;
};
static JxsCamUi* g_jxs_ui = nullptr;

// Multicast group for a camera: "<base>.<index>". base empty -> the shared
// JC_GROUP_BASE_DEFAULT (239.1.1). Derived from the SAME jxs_controls.* the OBS
// plugin uses, so the two consumers can never disagree. See docs/multicast-mode.md.
static std::string jxs_group_addr(const std::string& base, int idx)
{
  char out[32] = {0};
  if (!jc_group_addr(base.empty() ? nullptr : base.c_str(), idx, out, sizeof out))
    snprintf(out, sizeof out, "%s.%d", JC_GROUP_BASE_DEFAULT, JC_GROUP_INDEX_DEFAULT);
  return out;
}
static Fl_Window* g_jxs_controls_win = nullptr;

static void jxs_scan_results_cb(void*);

// Run the browse off the UI thread: jxs_discovery_scan() blocks for its window, and a
// frozen UI is not an acceptable way to look for cameras.
static void* jxs_scan_thread(void*)
{
  JxsCamUi* ui = g_jxs_ui;
  ui->n_results = jxs_discovery_scan(ui->results, JXS_DISC_MAX_CAMERAS, 1500);
  ui->scanning.store(false);
  Fl::awake(jxs_scan_results_cb, ui);
  return nullptr;
}

static void jxs_scan_results_cb(void* data)
{
  JxsCamUi* ui = static_cast<JxsCamUi*>(data);
  ui->found->clear();
  for (int k = 0; k < ui->n_results; ++k) {
    const jxs_discovered_camera_t& c = ui->results[k];
    if (c.address[0] == '\0')
      continue;  // an advertisement with no A record is not usable
    ui->found->add(fmt::format("{} | {} | api {} | {} {}", 
                               c.name[0] ? c.name : c.instance, c.address,
                               c.api_port, c.codec, c.resolution).c_str());
  }
  if (ui->n_results == 0)
    ui->found->add("(no cameras answered - type an address below)");
  logAppend(fmt::format("JXS discovery: {} camera(s)", ui->n_results));
}

static void jxs_found_pick_cb(Fl_Widget* w, void*)
{
  Fl_Browser* b = static_cast<Fl_Browser*>(w);
  const int v = b->value() - 1;
  if (!g_jxs_ui || v < 0 || v >= g_jxs_ui->n_results)
    return;
  const jxs_discovered_camera_t& c = g_jxs_ui->results[v];
  // The A-record address, never a re-resolved .local name: the name can answer with an
  // unreachable global IPv6 first.
  g_jxs_ui->host->value(c.address);
  g_jxs_ui->api->value(c.api_port);
  g_jxs_ui->name->value(c.name[0] ? c.name : c.instance);
}

static void jxs_scan_clicked_cb(Fl_Button*, void*)
{
  if (!g_jxs_ui || g_jxs_ui->scanning.load())
    return;
  g_jxs_ui->scanning.store(true);
  g_jxs_ui->found->clear();
  g_jxs_ui->found->add("scanning _obr-cam._udp ...");
  pthread_t t;
  pthread_create(&t, nullptr, jxs_scan_thread, nullptr);
  pthread_detach(t);
}

// This host's address on the CAMERA's subnet, found the way the OBS plugin finds
// it: a UDP connect() toward the camera picks the route (and sends nothing), and
// getsockname then reports the local address that route uses. Enumerating
// interfaces would pick the wrong NIC on a multi-homed box; the route-based
// answer cannot.
static std::string local_addr_toward(const std::string& ip)
{
  struct in_addr dst{};
  if (inet_pton(AF_INET, ip.c_str(), &dst) != 1)
    return {};
  const int fd = socket(AF_INET, SOCK_DGRAM, 0);
  if (fd < 0)
    return {};
  struct sockaddr_in sa{};
  sa.sin_family = AF_INET;
  sa.sin_addr = dst;
  sa.sin_port = htons(9);  // discard port: connect() sends nothing
  if (connect(fd, reinterpret_cast<struct sockaddr*>(&sa), sizeof sa) != 0) {
    close(fd);
    return {};
  }
  struct sockaddr_in local{};
  socklen_t len = sizeof local;
  if (getsockname(fd, reinterpret_cast<struct sockaddr*>(&local), &len) != 0) {
    close(fd);
    return {};
  }
  close(fd);
  char buf[INET_ADDRSTRLEN] = {};
  if (!inet_ntop(AF_INET, &local.sin_addr, buf, sizeof buf))
    return {};
  return buf;
}

// Multicast receive socket for one JXS camera on the SHARED port. Linux's default
// IP_MULTICAST_ALL=1 makes every socket bound to a port receive EVERY group's
// datagrams, so two cameras on one port cross-deliver TS into each other's demux
// ("Could not decode stream" + a dead tile); measured live: a socket joined to the
// Pi's group also saw the phone's datagrams. Setting IP_MULTICAST_ALL=0 pins the
// socket to the groups it actually joined (IP_ADD_MEMBERSHIP below), which keeps
// each camera's stream in its own demux on the single port (one ufw rule).
// Classic udpsrc has no property for this, so the app builds the socket and hands
// it to the element via the "socket" property.
static int make_multicast_socket(const std::string& group, uint16_t port)
{
  const int fd = socket(AF_INET, SOCK_DGRAM, 0);
  if (fd < 0)
    return -1;
  int one = 1;
  setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
  int mc_all = 0;  // IP_MULTICAST_ALL: 0 = only groups we joined
  setsockopt(fd, IPPROTO_IP, IP_MULTICAST_ALL, &mc_all, sizeof mc_all);
  struct sockaddr_in sa{};
  sa.sin_family = AF_INET;
  sa.sin_port = htons(port);
  sa.sin_addr.s_addr = htonl(INADDR_ANY);
  if (bind(fd, reinterpret_cast<struct sockaddr*>(&sa), sizeof sa) != 0) {
    close(fd);
    return -1;
  }
  // Join on the interface with a route toward the camera, like local_addr_toward.
  const std::string local = local_addr_toward(group);
  struct ip_mreqn mreq{};
  if (inet_pton(AF_INET, group.c_str(), &mreq.imr_multiaddr) != 1 ||
      inet_pton(AF_INET, local.c_str(), &mreq.imr_address) != 1) {
    close(fd);
    return -1;
  }
  mreq.imr_ifindex = 0;  // kernel picks the interface from imr_address
  if (setsockopt(fd, IPPROTO_IP, IP_ADD_MEMBERSHIP, &mreq, sizeof mreq) != 0) {
    close(fd);
    return -1;
  }
  return fd;
}

static void jxs_add_camera_cb(Fl_Button*, void*)
{
  if (!g_jxs_ui)
    return;
  const char* host = g_jxs_ui->host->value();
  if (!host || *host == '\0') {
    logAppend("Error: enter the camera's address before adding it.");
    return;
  }
  osr::JxsCamera cam;
  cam.host = host;
  // The node's control API default is 8080; a typed-in address (no scan) has no
  // api_port, and without one the camera can never be steered to this recorder -
  // the "added but no video ever arrives" failure. Default it, and store it, so
  // the steer and the controls pane both work for typed addresses.
  cam.api_port = g_jxs_ui->api->value() && *(g_jxs_ui->api->value())
                     ? g_jxs_ui->api->value()
                     : "8080";
  cam.name = g_jxs_ui->name->value() ? g_jxs_ui->name->value() : "";

  // Transport. Unicast: one port per camera (first free from 5001), exactly as
  // before. Multicast: ONE fixed port for every camera, each camera on its own
  // GROUP derived by incrementing the base block - which is what IGMP snooping
  // acts on. See docs/multicast-mode.md.
  cam.transport = (g_jxs_ui->transport && g_jxs_ui->transport->value() == 1)
                      ? osr::JxsTransport::Multicast
                      : osr::JxsTransport::Unicast;
  if (cam.transport == osr::JxsTransport::Multicast) {
    const std::string base = g_jxs_ui->group_base && g_jxs_ui->group_base->value() &&
                                     *g_jxs_ui->group_base->value()
                                 ? g_jxs_ui->group_base->value()
                                 : JC_GROUP_BASE_DEFAULT;
    // The index is a config: honour a typed value; otherwise auto-assign from
    // the groups the LAN already reports (read-only), keeping this camera's own
    // group when a scan found it.
    bool have_index = g_jxs_ui->group_index && g_jxs_ui->group_index->value() &&
                      *g_jxs_ui->group_index->value();
    int idx = have_index ? atoi(g_jxs_ui->group_index->value())
                         : JC_GROUP_INDEX_DEFAULT;
    if (!have_index) {
      jc_cam_endpoint_t eps[JXS_DISC_MAX_CAMERAS];
      int n = 0;
      const char* self = "";
      for (int r = 0; r < g_jxs_ui->n_results && n < JXS_DISC_MAX_CAMERAS; ++r) {
        const jxs_discovered_camera_t& c = g_jxs_ui->results[r];
        if (!c.address[0]) continue;
        snprintf(eps[n].instance, sizeof eps[n].instance, "%s", c.instance);
        snprintf(eps[n].address, sizeof eps[n].address, "%s", c.address);
        snprintf(eps[n].api_port, sizeof eps[n].api_port, "%s", c.api_port);
        if (strcmp(c.address, cam.host.c_str()) == 0) self = eps[n].instance;
        n++;
      }
      jc_autopick_group_index(base.c_str(), eps, n, self, nullptr, &idx);
    }
    cam.port = JC_MCAST_DEFAULT_PORT;  // shared by every camera: NOT per camera
    cam.group_index = idx;
    cam.group = jxs_group_addr(base, idx);
  } else {
    // Every JXS camera gets its OWN receive port on this recorder. The port is
    // derived, never typed: the first free port from 5001, and the steer below
    // tells the camera to send to exactly that port. Two cameras on the same port
    // both bind the same udpsrc (SO_REUSEADDR) and silently starve each other -
    // that is a source "added but never showing" in one line.
    cam.port = 5001;
    for (const auto& other : osr::config.jxs_cameras) {
      if (other.transport == osr::JxsTransport::Unicast && other.port >= cam.port)
        cam.port = other.port + 1;
    }
  }
  // Does this camera's stream actually carry the ST 302M audio elementary stream?
  // Ask the node, do NOT assume: a video-only camera wired into the preview's
  // audiomixer never delivers, and a silent mixer input blocks the WHOLE preview
  // from prerolling (checkerboard tiles + a Stop that wedges the UI thread). The
  // default in JxsCamera is true, so a camera that never answers must NOT stay
  // audio-claimed - unreachable here means video-only, and the preview/record
  // pipelines are built from what the stream reports, not from a guess.
  {
    const std::string url = fmt::format("http://{}:{}", cam.host, cam.api_port);
    bool has_audio = false;
    jc_node_status_audio(url.c_str(), &has_audio);
    cam.audio = has_audio;
    logAppend(fmt::format("JXS camera {}: stream audio {} ({})",
                          cam.host, has_audio ? "present" : "absent", url));
  }

  // The filename the recording uses. The operator's filename field wins when set; the
  // camera's own label is the fallback, so a take is never blocked on typing one.
  const char* fn = osr::app.ui->filename_input->value();
  std::string base = (fn && *fn) ? fn
                                 : (cam.name.empty() ? fmt::format("jxs_{}", cam.host)
                                                     : cam.name);
  osr::config.jxs_cameras.push_back(cam);
  osr::config.filenames.push_back(base);
  // The new camera's slot must not inherit a previous camera's measured range - and adding shifts
  // nothing here, but removal later will, so every slot's answer is dropped whenever the set moves.
  reset_jxs_declared_ranges();
  osr::app.ui->filename_input->value("");
  update_source_list();
  const std::string chan = cam.transport == osr::JxsTransport::Multicast
                               ? cam.group
                               : fmt::format("{}:{}", cam.host, cam.port);
  logAppend(fmt::format("JXS camera added: {} ({} {}) (slot {} of {})",
                        cam.host,
                        cam.transport == osr::JxsTransport::Multicast ? "multicast" : "unicast",
                        chan,
                        osr::config.jxs_cameras.size() - 1 + osr::config.video_sources.size(),
                        camera_count()));

  // A camera that was just selected must send its stream to THIS machine, or the
  // preview and every recording of it stay black - the operator picked it, so
  // "point it at me" is the intent, not a surprise. The dest is DERIVED, never
  // typed, exactly as the OBS plugin does: multicast -> the camera's GROUP on the
  // one shared port; unicast -> this host's address on the camera's subnet and the
  // camera's own port. Each camera has a distinct channel either way (a group in
  // multicast, a port in unicast), so one recorder serves several cameras without
  // their sockets fighting for the same datagrams.
  {
    const std::string url = fmt::format("http://{}:{}", cam.host, cam.api_port);
    const std::string port_str = fmt::format("{}", cam.port);
    const bool multicast = cam.transport == osr::JxsTransport::Multicast;
    const std::string dest = multicast ? cam.group : local_addr_toward(cam.host);
    char resp[JC_TEXT_MAX] = {0};
    char err[JC_ERR_MAX] = {0};
    if (dest.empty()) {
      logAppend(fmt::format(
          "JXS camera {}: could not determine this host's address toward it - "
          "set its destination in the Controls pane instead.", cam.host));
    } else if (jc_node_set_destination(url.c_str(), dest.c_str(), port_str.c_str(),
                                       resp, sizeof resp, err, sizeof err)) {
      logAppend(fmt::format("JXS camera {} steered to {}{} (its stream restarts)",
                            cam.host, dest,
                            multicast ? "" : fmt::format(":{}", cam.port)));
    } else {
      logAppend(fmt::format("JXS camera {}: could not steer its destination - {}",
                            cam.host, err));
    }
  }
}

void btn_jxs_cameras_cb(Fl_Button*, void*)
{
  if (!g_jxs_ui) {
    g_jxs_ui = new JxsCamUi;
    g_jxs_ui->win = new Fl_Window(560, 335, "JPEG XS cameras");
    new Fl_Box(15, 10, 530, 20, "Find cameras on the LAN, or type an address");
    g_jxs_ui->found = new Fl_Browser(15, 35, 530, 130);
    g_jxs_ui->found->type(FL_HOLD_BROWSER);
    g_jxs_ui->found->callback(jxs_found_pick_cb);
    Fl_Button* scan = new Fl_Button(15, 175, 120, 25, "Scan");
    scan->callback((Fl_Callback*)jxs_scan_clicked_cb);
    new Fl_Box(150, 175, 395, 25, "(discovery is a convenience - typing works without it)");
    g_jxs_ui->host = new Fl_Input(95, 210, 200, 25, "Address");
    g_jxs_ui->api = new Fl_Input(370, 210, 100, 25, "API port");
    g_jxs_ui->name = new Fl_Input(95, 245, 200, 25, "Label");
    // Transport: unicast (one port per camera, the classic path) or multicast
    // (ONE fixed port for every camera, separated by GROUP). See docs/multicast-mode.md.
    g_jxs_ui->transport = new Fl_Choice(95, 275, 200, 25, "Transport");
    g_jxs_ui->transport->add("Unicast|Multicast");
    g_jxs_ui->transport->value(0);
    g_jxs_ui->group_base = new Fl_Input(95, 305, 110, 25, "Group base");
    g_jxs_ui->group_base->value(JC_GROUP_BASE_DEFAULT);
    g_jxs_ui->group_index = new Fl_Input(300, 305, 60, 25, "index");
    g_jxs_ui->group_index->tooltip(
        "Multicast only. Blank = auto-assign the lowest free index from the LAN.");
    Fl_Button* add = new Fl_Button(370, 245, 100, 25, "Add camera");
    add->callback((Fl_Callback*)jxs_add_camera_cb);
    g_jxs_ui->win->end();
  }
  g_jxs_ui->win->show();
}

// ---- the controls pane -----------------------------------------------------
// The widget mapping is the descriptor's own classification (jc_widget_t), which the
// OBS plugin imports for the same purpose: Bool -> checkbox, Integer32 with an enum ->
// dropdown, bounded Integer32 -> slider, Float -> value slider, arrays -> token text.
// A control the camera reports as restart_required is shown disabled: it is read-only
// on the wire, so offering it would be a lie.
struct JxsCtrlWidget
{
  jc_widget_t kind;
  Fl_Widget* w;
  char name[JC_NAME_MAX];
};
static std::vector<JxsCtrlWidget> g_jxs_ctrl_widgets;

static char g_jxs_ctrl_url[JC_TEXT_MAX] = {0};
static Fl_Input* g_jxs_dest_input = nullptr;
static Fl_Input* g_jxs_dest_port = nullptr;

static void jxs_controls_apply_cb(Fl_Button*, void*)
{
  if (g_jxs_ctrl_url[0] == '\0')
    return;
  std::string body = "{";
  bool first = true;
  for (const JxsCtrlWidget& row : g_jxs_ctrl_widgets) {
    // A restart_required control is read-only on the wire and is shown deactivated;
    // deactivated widgets are never POSTed.
    if (!row.w || !row.w->active())
      continue;
    std::string val;
    if (row.kind == JC_W_BOOL) {
      val = static_cast<Fl_Check_Button*>(row.w)->value() ? "true" : "false";
    } else if (row.kind == JC_W_ENUM) {
      Fl_Choice* ch = static_cast<Fl_Choice*>(row.w);
      val = fmt::format("{}", reinterpret_cast<intptr_t>(
                                  ch->menu()[ch->value()].user_data()));
    } else if (row.kind == JC_W_INT) {
      val = fmt::format("{}", static_cast<long>(static_cast<Fl_Slider*>(row.w)->value()));
    } else if (row.kind == JC_W_FLOAT) {
      val = fmt::format("{}", static_cast<double>(
                                  static_cast<Fl_Value_Slider*>(row.w)->value()));
    } else {
      // Array controls travel as the camera's own canonical token string.
      char canon[JC_TEXT_MAX];
      jc_canon_tokens(static_cast<Fl_Input*>(row.w)->value(), canon, sizeof canon);
      val = fmt::format("\"{}\"", canon);
    }
    body += fmt::format("{}\"{}\": {}", first ? "" : ", ", row.name, val);
    first = false;
  }
  body += "}";
  if (first) {
    logAppend("Controls: nothing to apply.");
    return;
  }
  char resp[JC_TEXT_MAX] = {0};
  char err[JC_ERR_MAX] = {0};
  if (jc_controls_post(g_jxs_ctrl_url, body.c_str(), resp, sizeof resp, err, sizeof err))
    logAppend(fmt::format("Controls: applied {}", body));
  else
    logAppend(fmt::format("Controls: apply failed ({})", err));
}

// ---- Match cameras: the image-match walk, driven from the controls pane ----
//
// The phone's tone curve is the ONLY matchable part of its picture (see jxs_controls.h for why:
// TonemapMode 0 hands the device OUR points, and modes 1/2 are the device's own adaptive map,
// which is not copyable). The match is CLOSED-LOOP and cannot be derived: the control points are
// not in the output-code domain - a measured 4% change to the mid-curve moved the image mean by 44
// code values - so every candidate must be APPLIED and then MEASURED off the picture that comes
// back. That is why this drives the vendored helper's plan walk (jc_match_begin/next/best/apply)
// rather than computing a curve from a histogram: the histogram would be wrong however carefully it
// were derived. The search, the curve family and the score all live in the helper; this file owns
// only the one thing the helper cannot - getting a real frame out of each camera between steps.
//
// Where the frames come from: the recorder ALREADY decodes both cameras' JPEG XS streams - each
// camera's capture pipeline turns its transport stream into YUY2 and hands it to intersink_v. This
// taps that same pad with a probe (no second receiver, no second decoder) and remembers the latest
// frame per camera. Only while a walk runs: idle previews must not pay to copy every frame.
//
// Threading: the walk applies a curve, waits ~2 s for the change to be live and a fresh frame to
// arrive, measures, and repeats for every candidate - about 20 s in total. None of that may block
// the UI thread, so the walk runs on its own thread and only the RESULT is marshalled back.

struct MatchFrame
{
  std::mutex mtx;
  std::vector<unsigned char> data;  // a copy of the mapped frame (row stride preserved)
  int w = 0, h = 0, stride = 0;
  uint64_t seq = 0;                 // bumped once per captured frame
};
static MatchFrame g_match_frame[osr::App::kMaxSources];
// Set only while a walk runs: the probe copies frames ONLY then, so a normal preview pays nothing.
static std::atomic_bool g_match_capture_on{false};

// Tap one camera's decoded video (the intersink_v sink pad): remember the newest YUY2 frame while a
// walk is running. YUY2 is what every branch of this app hands to intersink_v, so the luma is the
// first byte of each 2-pixel group in every row - see luma_from_yuy2().
static GstPadProbeReturn match_frame_cb(GstPad* pad, GstPadProbeInfo* info, gpointer user_data)
{
  if (!g_match_capture_on.load(std::memory_order_relaxed))
    return GST_PAD_PROBE_OK;
  const size_t i = static_cast<size_t>(GPOINTER_TO_INT(user_data));
  if (i >= osr::app.kMaxSources)
    return GST_PAD_PROBE_OK;
  GstBuffer* buf = GST_PAD_PROBE_INFO_BUFFER(info);
  if (buf == nullptr)
    return GST_PAD_PROBE_OK;
  GstCaps* caps = gst_pad_get_current_caps(pad);
  GstVideoInfo vi;
  if (caps == nullptr || !gst_video_info_from_caps(&vi, caps))
  {
    if (caps) gst_caps_unref(caps);
    return GST_PAD_PROBE_OK;
  }
  gst_caps_unref(caps);
  // The helper's luma input is a packed 8-bit plane; a format this tap cannot pack is skipped
  // rather than mis-read (every producer here negotiates YUY2, but never assume it on the pad).
  if (GST_VIDEO_INFO_FORMAT(&vi) != GST_VIDEO_FORMAT_YUY2)
    return GST_PAD_PROBE_OK;
  GstMapInfo map;
  if (!gst_buffer_map(buf, &map, GST_MAP_READ))
    return GST_PAD_PROBE_OK;
  if (map.size > 0)
  {
    MatchFrame& f = g_match_frame[i];
    std::lock_guard<std::mutex> lk(f.mtx);
    f.data.assign(map.data, map.data + map.size);
    f.w = GST_VIDEO_INFO_WIDTH(&vi);
    f.h = GST_VIDEO_INFO_HEIGHT(&vi);
    f.stride = GST_VIDEO_INFO_PLANE_STRIDE(&vi, 0);
    f.seq++;
  }
  gst_buffer_unmap(buf, &map);
  return GST_PAD_PROBE_OK;
}

// Pack a contiguous luma plane out of a YUY2 frame (Y is the first byte of each 2-pixel group), then
// let the VENDORED helper build the statistics - the histogram, the percentiles and the score live
// in ONE place and are never re-derived here.
static bool luma_from_yuy2(jc_luma_t* out, const unsigned char* yuy2, int w, int h, int stride)
{
  if (stride <= 0)
    stride = w * 2;
  std::vector<unsigned char> y(static_cast<size_t>(w) * static_cast<size_t>(h));
  for (int r = 0; r < h; ++r)
  {
    const unsigned char* s = yuy2 + static_cast<size_t>(r) * stride;
    unsigned char* d = y.data() + static_cast<size_t>(r) * w;
    for (int c = 0; c < w; ++c)
      d[c] = s[static_cast<size_t>(2) * c];
  }
  return jc_luma_from_plane(out, y.data(), w, h, w);
}

// Snapshot the newest frame for a camera into a local copy, so the packing runs outside the lock.
static bool grab_luma(size_t i, jc_luma_t* out)
{
  MatchFrame& f = g_match_frame[i];
  std::vector<unsigned char> copy;
  int w = 0, h = 0, stride = 0;
  {
    std::lock_guard<std::mutex> lk(f.mtx);
    if (f.seq == 0 || f.w <= 0 || f.h <= 0)
      return false;
    copy = f.data;
    w = f.w;
    h = f.h;
    stride = f.stride;
  }
  return luma_from_yuy2(out, copy.data(), w, h, stride);
}

static uint64_t frame_seq(size_t i)
{
  std::lock_guard<std::mutex> lk(g_match_frame[i].mtx);
  return g_match_frame[i].seq;
}

// Wait until a NEWER frame than `after` has arrived for this camera, or the deadline passes. A stale
// frame is worse than none: it would score the curve BEFORE the one just applied.
static bool wait_frame_after(size_t i, uint64_t after, int timeout_ms)
{
  const gint64 deadline = g_get_monotonic_time() + static_cast<gint64>(timeout_ms) * 1000;
  while (g_get_monotonic_time() < deadline)
  {
    if (frame_seq(i) > after)
      return true;
    g_usleep(50 * 1000);
  }
  return false;
}

// What the walk found, handed back to the UI thread.
struct MatchOutcome
{
  bool ok = false;            // the walk completed and a best curve was applied
  bool absolute = false;
  size_t slave = 0, master = 0;
  double residual = -1.0;     // the score of the applied best curve, measured fresh (-1 = unmeasured)
  double best_score = -1.0;   // the score the plan itself recorded for the best candidate
  double gamma = 0.0, contrast = 0.0;
  int tested = 0;
  std::string message;
};

static std::atomic_bool g_match_running{false};
static std::thread g_match_thread;
static size_t g_match_slave_slot = 0;             // the camera whose controls pane this is
static Fl_Choice* g_match_master_choice = nullptr;  // the master picker in that pane
static Fl_Check_Button* g_match_absolute = nullptr; // "exposure pinned -> absolute score"
static Fl_Box* g_match_status = nullptr;           // the pane's residual read-out
static Fl_Button* g_match_button = nullptr;

// The Raspberry Pi node is the intended default master: it is the flatter reference (contrast and
// sharpness only, no tone curve), so the phone is matched against it rather than the other way
// round. This is its address on this rig's video LAN; a rig without a node at it just gets the first
// other camera, and the operator can pick another master either way.
static const char kMatchDefaultMasterHost[] = "192.168.1.118";

static int default_master_choice_index(size_t slave_slot)
{
  int first_other = -1;
  for (size_t i = 0; i < camera_count(); ++i)
  {
    if (i == slave_slot)
      continue;
    if (first_other < 0)
      first_other = static_cast<int>(i);
    if (is_jxs(i) && jxs_of(i).host == kMatchDefaultMasterHost)
      return static_cast<int>(i);
  }
  return first_other;
}

// The walk, on its own thread: apply -> settle -> measure -> feed the score back, for every
// candidate in the helper's plan. The helper owns the SEARCH; this owns the timing and the frames.
static void run_match_walk(size_t slave_slot, size_t master_slot, bool absolute)
{
  MatchOutcome out;
  out.slave = slave_slot;
  out.master = master_slot;
  out.absolute = absolute;

  const std::string slave_url = jxs_base_url(slave_slot);

  auto finish = [&out]() {
    g_match_capture_on.store(false);
    logAppend(out.message);
    MatchOutcome* heap = new MatchOutcome(out);
    Fl::awake([](void* p) {
      auto* o = static_cast<MatchOutcome*>(p);
      if (g_match_button)
        g_match_button->activate();
      if (g_match_status)
      {
        const std::string s = o->ok
            ? fmt::format("Match: residual {:.2f} (gamma {:.2f}, contrast {:.2f})",
                          o->residual, o->gamma, o->contrast)
            : std::string("Match: no result");
        g_match_status->copy_label(s.c_str());
        g_match_status->redraw();
      }
      delete o;
    }, heap);
    g_match_running.store(false);
  };

  if (slave_url.empty())
  {
    out.message = fmt::format("Match: {} has no API port known - a tone curve can only be applied to a node.",
                              camera_label(slave_slot));
    finish();
    return;
  }

  g_match_capture_on.store(true);
  // Give the probes a moment to catch a first frame from each camera before the walk starts.
  wait_frame_after(slave_slot, frame_seq(slave_slot), 4000);
  wait_frame_after(master_slot, frame_seq(master_slot), 4000);

  jc_match_t state;
  jc_match_begin(&state);
  jc_curve_t cand;
  double score = 0.0;
  int tested = 0;

  while (jc_match_next(&state, score, &cand))
  {
    if (osr::app.stop_requested.load())
      break;
    char err[JC_ERR_MAX] = {0};
    if (!jc_match_apply(slave_url.c_str(), &cand, err, sizeof err))
    {
      // A refused candidate is scored negative so the plan never keeps it as best (see the header).
      logAppend(fmt::format("Match: candidate gamma {:.2f} contrast {:.2f} not applied ({})",
                            state.gamma, state.contrast, err));
      score = -1.0;
      continue;
    }
    // The baseline is taken AFTER the apply: the frames scored must postdate the curve, not predate
    // it. ~2 s lets the hot change land and the decoder catch up, exactly as the brief requires.
    const uint64_t slave_after = frame_seq(slave_slot);
    const uint64_t master_after = frame_seq(master_slot);
    g_usleep(2000 * 1000);
    wait_frame_after(slave_slot, slave_after, 3000);
    wait_frame_after(master_slot, master_after, 3000);

    jc_luma_t master_luma, slave_luma;
    const bool have = grab_luma(master_slot, &master_luma) && grab_luma(slave_slot, &slave_luma);
    score = have ? jc_match_score(&master_luma, &slave_luma, absolute) : -1.0;
    tested++;
    logAppend(fmt::format(
        "Match: candidate {} gamma {:.2f} contrast {:.2f} -> {}",
        tested, state.gamma, state.contrast,
        score < 0.0 ? std::string("no usable frame") : fmt::format("score {:.2f}", score)));
  }

  out.tested = tested;
  if (state.best_score < 0.0)
  {
    out.message = fmt::format("Match: no candidate produced a usable score ({} tested) - the camera's "
                              "curve was left as it was.", tested);
    finish();
    return;
  }

  jc_curve_t best;
  jc_match_best(&state, &best);
  char err[JC_ERR_MAX] = {0};
  if (!jc_match_apply(slave_url.c_str(), &best, err, sizeof err))
  {
    out.message = fmt::format("Match: best curve (gamma {:.2f}, contrast {:.2f}) could not be applied ({})",
                              state.best_gamma, state.best_contrast, err);
    finish();
    return;
  }

  // Final residual: the best curve is applied and measured again off fresh frames, so the reported
  // number is what the picture scores NOW - not what the plan recorded mid-walk.
  const uint64_t slave_after = frame_seq(slave_slot);
  const uint64_t master_after = frame_seq(master_slot);
  g_usleep(2000 * 1000);
  wait_frame_after(slave_slot, slave_after, 3000);
  wait_frame_after(master_slot, master_after, 3000);
  jc_luma_t master_luma, slave_luma;
  const bool have = grab_luma(master_slot, &master_luma) && grab_luma(slave_slot, &slave_luma);

  out.ok = true;
  out.gamma = state.best_gamma;
  out.contrast = state.best_contrast;
  out.best_score = state.best_score;
  out.residual = have ? jc_match_score(&master_luma, &slave_luma, absolute) : state.best_score;
  out.message = fmt::format(
      "Match: {} (slave) vs {} (master) - applied gamma {:.2f} contrast {:.2f}; residual {:.2f} "
      "({} score, {} candidate(s) tested)",
      camera_label(slave_slot), camera_label(master_slot), out.gamma, out.contrast, out.residual,
      absolute ? "absolute" : "shape", tested);
  finish();
}

static void jxs_controls_match_cb(Fl_Button*, void*)
{
  if (g_match_running.load())
  {
    logAppend("Match: a walk is already running.");
    return;
  }
  const size_t slave = g_match_slave_slot;
  if (!is_jxs(slave) || jxs_base_url(slave).empty())
  {
    logAppend("Match: the selected camera has no control API - a tone curve can only be applied to a node.");
    return;
  }
  if (camera_count() < 2)
  {
    logAppend("Match: need at least one other camera to use as the master reference.");
    return;
  }
  if (!g_match_master_choice || g_match_master_choice->size() <= 0)
    return;
  const int msel = g_match_master_choice->value();
  const size_t master =
      static_cast<size_t>(reinterpret_cast<intptr_t>(g_match_master_choice->menu()[msel].user_data()));
  if (master == slave)
  {
    logAppend("Match: the master must be a different camera from the one being matched.");
    return;
  }
  if (!osr::app.session_active.load() && !osr::app.preview_running.load())
  {
    logAppend("Match: start the preview first - the walk measures the decoded pictures, and there are "
              "none while the cameras are idle.");
    return;
  }
  // Shape is the honest measure while both cameras are on auto exposure (the absolute level is the
  // AEs' business, not the curve's); absolute is the honest one once the exposure is pinned.
  const bool absolute = g_match_absolute && g_match_absolute->value() != 0;

  if (g_match_thread.joinable())
    g_match_thread.join();  // the previous walk has finished (g_match_running guarded above)
  g_match_running.store(true);
  if (g_match_button)
    g_match_button->deactivate();
  if (g_match_status)
  {
    g_match_status->copy_label("Match: running...");
    g_match_status->redraw();
  }
  logAppend(fmt::format("Match: starting the walk - {} (slave) against {} (master), {} score, about 20 s.",
                        camera_label(slave), camera_label(master), absolute ? "absolute" : "shape"));
  g_match_thread = std::thread(run_match_walk, slave, master, absolute);
}

static void jxs_controls_dest_cb(Fl_Button*, void*);

void btn_jxs_controls_cb(Fl_Button*, void*)
{
  const int sel = osr::app.ui->browser_source_list->value() - 1;
  if (sel < 0 || !is_jxs(static_cast<size_t>(sel))) {
    logAppend("Controls: select a JXS camera in the source list first.");
    return;
  }
  const std::string url = jxs_base_url(static_cast<size_t>(sel));
  if (url.empty()) {
    logAppend("Controls: this camera has no API port known - add it via JXS Cameras... after a scan.");
    return;
  }
  jc_controls_t ctrl;
  if (!jc_controls_fetch(url.c_str(), &ctrl)) {
    logAppend(fmt::format("Controls: cannot reach {} ({})", url, ctrl.error));
    return;
  }
  g_strlcpy(g_jxs_ctrl_url, url.c_str(), sizeof g_jxs_ctrl_url);

  if (g_jxs_controls_win) {
    g_jxs_controls_win->hide();
    Fl::delete_widget(g_jxs_controls_win);
    g_jxs_controls_win = nullptr;
    // The match widgets die with the window; a walk that is still finishing must not touch freed
    // widgets, so drop the pointers here and let the rebuild below re-point them.
    g_match_master_choice = nullptr;
    g_match_absolute = nullptr;
    g_match_status = nullptr;
    g_match_button = nullptr;
  }
  g_jxs_ctrl_widgets.clear();

  const int rows = ctrl.n_fields + 9;
  // FLTK stores the label POINTER (Fl_Widget::label(const char*)); only
  // copy_label() duplicates it. ctrl is a local, so every label that points
  // into it must be copied - the window outlives this function.
  const std::string title = fmt::format("Controls - {}", camera_label(static_cast<size_t>(sel)));
  Fl_Window* win = new Fl_Window(520, 40 + rows * 30 + 40, title.c_str());
  win->copy_label(title.c_str());
  int y = 30;
  if (!ctrl.status[0])
    snprintf(ctrl.status, sizeof ctrl.status, "%d control(s), hot=%s", ctrl.n_fields,
             ctrl.hot ? "yes" : "no");
  Fl_Box* status_box = new Fl_Box(15, 5, 490, 20, ctrl.status);
  status_box->copy_label(ctrl.status);

  for (int f = 0; f < ctrl.n_fields; ++f) {
    jc_field_t* c = &ctrl.fields[f];
    JxsCtrlWidget row{};
    row.kind = c->widget;
    g_strlcpy(row.name, c->name, sizeof row.name);
    // A ZOOM control states WHAT it drives: the node discovered the mechanism
    // from the device (libcamera ScalerCrop / Camera2 zoom range / a UVC
    // V4L2_CID_ZOOM_ABSOLUTE lens). Same rule as the OBS plugin's pane, so an
    // operator cannot mistake a digital crop for a lens.
    //
    // The label is held in a std::string and NOT handed to FLTK as a borrowed
    // pointer: Fl_Widget::label(const char*) stores the pointer, and anything
    // scoped to this loop body is overwritten by the next field - that is what
    // blanked every row in the pane. Each widget gets its own copy below.
    std::string label(c->name);
    if (c->has_zoom_type)
      label = fmt::format("{}  |  {}{}{}", c->name, jc_zoom_type_label(c->zoom_type),
                          c->zoom_type_source[0] ? ": " : "", c->zoom_type_source);
    Fl_Widget* w = nullptr;
    if (c->widget == JC_W_BOOL) {
      Fl_Check_Button* b = new Fl_Check_Button(25, y, 300, 25, label.c_str());
      b->value(c->has_value && c->val_bool);
      w = b;
    } else if (c->widget == JC_W_ENUM) {
      Fl_Choice* ch = new Fl_Choice(180, y, 250, 25, label.c_str());
      for (int e = 0; e < c->enum_count; ++e)
        ch->add(c->enums[e].label, nullptr, nullptr,
                reinterpret_cast<void*>(static_cast<intptr_t>(c->enums[e].value)));
      for (int e = 0; e < c->enum_count; ++e)
        if (c->has_value && c->enums[e].value == static_cast<int>(c->val_num))
          ch->value(e);
      w = ch;
    } else if (c->widget == JC_W_INT) {
      Fl_Slider* s = new Fl_Slider(180, y, 250, 25, label.c_str());
      s->type(FL_HOR_NICE_SLIDER);
      // Fl_Slider::draw() draws the label inside its own box, which for a nice
      // slider is the ~17px handle - the text never survives there. An outside
      // alignment hands the label to the group, which draws it in the name
      // column on the left like every other row.
      s->align(FL_ALIGN_LEFT);
      s->bounds(c->has_min ? c->imin : 0, c->has_max ? c->imax : 100);
      s->step(1);
      s->value(c->has_value ? c->val_num : 0);
      w = s;
    } else if (c->widget == JC_W_FLOAT) {
      Fl_Value_Slider* s = new Fl_Value_Slider(180, y, 250, 25, label.c_str());
      s->type(FL_HOR_NICE_SLIDER);
      s->align(FL_ALIGN_LEFT);
      s->bounds(c->has_min ? c->fmin : 0.0, c->has_max ? c->fmax : 1.0);
      s->value(c->has_value ? c->val_num : 0.0);
      w = s;
    } else {
      Fl_Input* in = new Fl_Input(180, y, 250, 25, label.c_str());
      if (c->has_value)
        in->value(c->val_text);
      w = in;
    }
    // copy_label() strdups; the widget frees its own copy when it is destroyed.
    w->copy_label(label.c_str());
    if (c->restart_required) {
      w->deactivate();
      w->tooltip("read-only while streaming: restart_required");
      const std::string rl = fmt::format("{} (restart required)", label);
      w->copy_label(rl.c_str());
    }
    row.w = w;
    g_jxs_ctrl_widgets.push_back(row);
    y += 30;
  }

  g_jxs_dest_input = new Fl_Input(180, y, 250, 25, "Stream destination");
  g_jxs_dest_port = new Fl_Input(445, y, 60, 25, "port");
  g_jxs_dest_port->value("5001");
  Fl_Button* setdest = new Fl_Button(180, y + 30, 130, 25, "Set destination");
  setdest->callback((Fl_Callback*)jxs_controls_dest_cb);
  Fl_Button* apply = new Fl_Button(320, y + 30, 110, 25, "Apply hot");
  apply->callback((Fl_Callback*)jxs_controls_apply_cb);
  new Fl_Box(25, y + 60, 480, 20,
             "restart_required controls are read-only; the destination restarts the stream.");

  // --- Match cameras: drive the shared image-match helper on THIS camera (the slave)
  //     against a chosen master. The Pi node is the intended master (the flatter reference).
  g_match_slave_slot = static_cast<size_t>(sel);
  g_match_master_choice = new Fl_Choice(180, y + 95, 250, 25, "Match master");
  g_match_master_choice->tooltip(
      "The camera to match TO. The Raspberry Pi node is the intended master: it is the flatter "
      "reference (no tone curve), so this camera is bent to it.");
  for (size_t i = 0; i < camera_count(); ++i)
  {
    if (i == g_match_slave_slot)
      continue;  // a camera cannot be its own master
    g_match_master_choice->add(camera_label(i).c_str(), nullptr, nullptr,
                               reinterpret_cast<void*>(static_cast<intptr_t>(i)));
  }
  const int def_master = default_master_choice_index(g_match_slave_slot);
  for (int e = 0; e < g_match_master_choice->size(); ++e)
    if (static_cast<int>(reinterpret_cast<intptr_t>(
            g_match_master_choice->menu()[e].user_data())) == def_master)
      g_match_master_choice->value(e);
  g_match_absolute = new Fl_Check_Button(25, y + 125, 340, 25, "Exposure pinned (score absolute)");
  g_match_absolute->tooltip(
      "Off while both cameras are on auto exposure: the shape score is the honest measure then. "
      "On once the exposure is pinned: the absolute score is.");
  g_match_button = new Fl_Button(180, y + 155, 130, 25, "Match cameras");
  g_match_button->callback((Fl_Callback*)jxs_controls_match_cb);
  g_match_status = new Fl_Box(25, y + 185, 480, 20, "Match: idle");
  g_match_status->copy_label("Match: idle");
  g_match_status->align(FL_ALIGN_LEFT | FL_ALIGN_INSIDE);

  win->end();
  g_jxs_controls_win = win;
  win->show();
  logAppend(fmt::format("Controls: {} fetched {} field(s), {} restart-required",
                        url, ctrl.n_fields, ctrl.n_restart));
}

static void jxs_controls_dest_cb(Fl_Button*, void*)
{
  if (g_jxs_ctrl_url[0] == '\0' || !g_jxs_dest_input)
    return;
  const char* dest = g_jxs_dest_input->value();
  const int port = g_jxs_dest_port ? atoi(g_jxs_dest_port->value()) : 5001;
  if (!dest || *dest == '\0') {
    logAppend("Controls: enter a destination address first.");
    return;
  }
  const std::string port_str = fmt::format("{}", port);
  char resp[JC_TEXT_MAX] = {0};
  char err[JC_ERR_MAX] = {0};
  if (jc_node_set_destination(g_jxs_ctrl_url, dest, port_str.c_str(), resp, sizeof resp,
                              err, sizeof err))
    logAppend(fmt::format("Controls: destination set to {}:{} (the camera restarts its stream)", dest, port));
  else
    logAppend(fmt::format("Controls: destination refused ({})", err));
}

void add_source_cb(Fl_Button*, void*)
{
  const char* filename = osr::app.ui->filename_input->value();
  if (!filename || strlen(filename) == 0) {
    logAppend("Error: Please enter a target filename before adding a source.");
    return;
  }

  osr::config.video_sources.push_back(osr::app.selected_video_device);
  osr::config.audio_sources.push_back(osr::app.selected_audio_device);
  osr::config.filenames.push_back(std::string(filename));

  osr::app.ui->filename_input->value("");
  update_source_list();
}

void remove_source_cb(Fl_Button*, void*)
{
  if (osr::app.ui->browser_source_list->value()) {
    const int idx = osr::app.ui->browser_source_list->value() - 1;
    // Which list a slot came from depends on its index: device cameras are the low
    // slots, network cameras the ones after them.
    if (static_cast<size_t>(idx) >= osr::config.video_sources.size()) {
      const size_t j = static_cast<size_t>(idx) - osr::config.video_sources.size();
      if (j < osr::config.jxs_cameras.size())
        osr::config.jxs_cameras.erase(osr::config.jxs_cameras.begin()
                                      + static_cast<ptrdiff_t>(j));
    } else {
      osr::config.video_sources.erase(osr::config.video_sources.begin()
                                      + static_cast<ptrdiff_t>(idx));
      osr::config.audio_sources.erase(osr::config.audio_sources.begin()
                                      + static_cast<ptrdiff_t>(idx));
    }
    osr::config.filenames.erase(osr::config.filenames.begin()
                                + static_cast<ptrdiff_t>(idx));
    // Removing a camera shifts every later one into a different slot, so every slot's measured
    // range is stale now.
    reset_jxs_declared_ranges();
    update_source_list();
  }
}

static void log_pipeline_error(const char* message, GError* error)
{
  logAppend(fmt::format("Error creating pipeline: {}\n", message));
  if (error) {
    logAppend(fmt::format("GError: {}\n", error->message));
    g_clear_error(&error);
  }
}

std::string escape(const char* src,
                   const std::set<char> escapee,
                   const char marker)
{
  std::string r;
  while (char c = *src++) {
    if (escapee.find(c) != escapee.end())
      r += marker;
    // r += c;
    r += c == '\n' ? 'n' : c;
  }
  return r;
}

// The VIDEO source is per-OS. Every source is a GstDevice discovered by the app's GstDeviceMonitor,
// so name the element that can open *that* device on this platform instead of hardcoding one OS's.
// The identity property differs too, and is read off the device's properties:
//   Windows Media Foundation : mfvideosrc / device.name
//   Linux                    : v4l2src    / device.path (/dev/videoN)
//
// The AUDIO source is not per-OS but per DEVICE PROVIDER: one machine's device list holds PipeWire
// nodes and raw ALSA devices side by side, and each needs its own element. It is therefore built
// from the device's own properties in audio_source_element() below, not by a platform macro.
#if defined(_WIN32)
#define OSR_VIDEO_SRC_HEAD "mfvideosrc do-timestamp=true device-name="
#define OSR_VIDEO_SRC_PROP "device.name"
#else
#define OSR_VIDEO_SRC_HEAD "v4l2src do-timestamp=true device="
#define OSR_VIDEO_SRC_PROP "device.path"
#endif

// The source element for a selected audio DEVICE, chosen from the device's own properties. The
// device list mixes provider sorts, and each sort needs its own element:
//
//   PipeWire node   object.serial + node.name, an ALSA card underneath. Named by node.name, NOT by
//                   serial: the serial changes when PipeWire restarts, and the stale one then fails
//                   the stream with "stream error: target not found" - which is the "audio not
//                   found" the recorder used to report.
//   raw ALSA device the ALSA provider's own entry: alsa.card/alsa.device and no serial at all.
//   Windows         one provider; wasapi2src with the device's strid.
//
// Naming the wrong one used to produce "target-object=" (an empty property) and with it an
// unparseable pipeline - GError: no element "audio" - which failed the camera's WHOLE capture
// pipeline, video included. An empty return means no known element can open this device; the caller
// then records video only instead of failing the camera.
static std::string audio_source_element(GstDevice* device)
{
  if (device == nullptr)
    return {};

  GstStructure* props = gst_device_get_properties(device);
  if (props == nullptr)
    return {};

  std::string element;

#if defined(_WIN32)
  const gchar* strid = gst_structure_get_string(props, "device.strid");
  if (strid != nullptr && *strid != '\0')
    element = fmt::format("wasapi2src do-timestamp=true low-latency=true device={}",
                          escape(strid, {' '}, '\\'));
#else
  const gchar* serial = gst_structure_get_string(props, "object.serial");
  const gchar* node_name = gst_structure_get_string(props, "node.name");
  gint card = -1;
  gint subdevice = 0;

  if (serial != nullptr && *serial != '\0')
  {
    const gchar* target = (node_name != nullptr && *node_name != '\0') ? node_name : serial;
    element = fmt::format("pipewiresrc do-timestamp=true target-object={}",
                          escape(target, {' '}, '\\'));
  }
  else if (gst_structure_get_int(props, "alsa.card", &card))
  {
    if (!gst_structure_get_int(props, "alsa.device", &subdevice))
      subdevice = 0;
    element = fmt::format("alsasrc do-timestamp=true device=hw:{},{}", card, subdevice);
  }
#endif

  gst_structure_free(props);
  return element;
}

// The source element for a selected VIDEO device: V4L, deliberately, not PipeWire. v4l2src hands on
// the device's own colour on its src caps (measured 2:4:5:1 on the MS2131, 2:4:7:1 on the UGREEN
// 65518), so the kernel node needs no graph in the path to report it - one owner of the unit, and
// what the recorder declares is what the device says.
static std::string video_source_element(GstDevice* device)
{
  if (device == nullptr)
    return {};

  GstStructure* props = gst_device_get_properties(device);
  if (props == nullptr)
    return {};

  std::string element;
#if defined(_WIN32)
  const gchar* name = gst_structure_get_string(props, "device.name");
  if (name != nullptr && *name != '\0')
    element = fmt::format("mfvideosrc do-timestamp=true name=capture_src device-name={}",
                          escape(name, {' '}, '\\'));
#else
  const gchar* path = gst_structure_get_string(props, "device.path");
  if (path == nullptr || *path == '\0')
    path = gst_structure_get_string(props, "api.v4l2.path");
  if (path != nullptr && *path != '\0')
    element = fmt::format("v4l2src do-timestamp=true name=capture_src device={}",
                          escape(path, {' '}, '\\'));
#endif

  gst_structure_free(props);
  return element;
}

// The colour the capture leg actually declares, in the device's own words, read off the negotiating
// pad once. This is the value a recording must be tagged with and the value the operator is owed:
// the GstDevice's caps list can arrive with no colour at all (v4l2deviceprovider, udev-probed=false)
// while v4l2src still signs the device's colorimetry on the stream, so the stream is asked.
static GstPadProbeReturn log_capture_caps_cb(GstPad* pad, GstPadProbeInfo*, gpointer user_data)
{
  const auto index = static_cast<size_t>(GPOINTER_TO_INT(user_data));
  GstCaps* caps = gst_pad_get_current_caps(pad);
  if (caps != nullptr && gst_caps_get_size(caps) > 0)
  {
    const gchar* colorimetry =
        gst_structure_get_string(gst_caps_get_structure(caps, 0), "colorimetry");
    logAppend(fmt::format("CAM {}: capture declares colorimetry={} (read from the device)", index,
                          colorimetry != nullptr ? colorimetry : "none signalled"));
  }
  if (caps != nullptr)
    gst_caps_unref(caps);
  return GST_PAD_PROBE_REMOVE;  // once: the caps of a v4l2 leg do not change mid-take
}

// Colour, in one place, for every pipeline that carries video (the preview and every encoder
// leg read this), so a recording cannot disagree with what the operator saw.
//
// The NAMED shorthand is used rather than a numeric tuple on purpose: "bt709" expands to
// limited range + BT.709 matrix/transfer/primaries. A numeric tuple carrying transfer=1 was
// measured landing as colour_transfer=linear in the file (GStreamer 1.28.6 + ffmpeg n9.0.1,
// 2026-10), while the shorthand gave bt709 everywhere.
//
// What the container does with it, measured on this rig:
//   MOV (qtmux)        matrix/transfer/primaries survive; range is DROPPED (color_range=unknown).
//                      QuickTime's colr box has no range field, so a limited-range recording
//                      cannot say so. No tag change fixes that - it is the container.
//   MKV (matroskamux)  all four survive, color_range=tv included. MKV is reachable for the
//                      codecs matroskamux lists - here ProRes, and FFV1.
// The old value was 2:4:0:1 - BT.601 matrix with an UNKNOWN transfer, an SD pairing - which
// reported color_space=smpte170m on a 1080p recording. HD over HDMI is BT.709, and this rig's
// own characterisation of the MS2131 capture unit calls it "Rec.709-limited".
//
// chroma-site dv is left ALONE on purpose: for 4:2:2 there is no vertical chroma subsampling for
// it to affect, and changing the site would move real chroma alignment, which needs a measurement
// of the capture units rather than a guess.
// The range a pipeline leg DECLARES, and the RangeMode -> ColorRange mapping, live in
// source/range_declaration.h: the mapping is what decides whether a measured full-range JPEG XS
// stream is tagged 1:3:5:1 or (as it used to be) 0:3:5:1, so it is shared with the test that pins
// it. Nothing below redefines it.

// The caps string every video leg is built from, with the colorimetry as a parameter rather than a
// policy: a leg that knows its colour (a measured JPEG XS stream, or a conversion this application
// performed) passes it in, and a leg that does not know it passes none.
static std::string make_video_caps_string_with_colorimetry(
    std::string colorimetry)
{
  return fmt::format(
      "video/x-raw, format=YUY2, width={}, height={}, framerate={}/1, "
      "pixel-aspect-ratio=1/1, colorimetry={}, chroma-site=dv",
      osr::config.v_width,
      osr::config.v_height,
      osr::config.v_framerate,
      colorimetry);
}

static std::string make_video_caps_string(ColorRange range)
{
  return make_video_caps_string_with_colorimetry(colorimetry_for(range));
}

// The caps for a leg that states nothing about the colour. Two callers, one reason:
//
//   * a capture DEVICE's leg. The device supplies the colour and v4l2src hands it on; naming one
//     here does not re-tag the samples, it refuses the stream - see device_advertised_colorimetry.
//   * a JPEG XS camera's leg before its stream has been MEASURED. Stating a range there would pin
//     what the producer has not declared, and the two legs then cannot intersect: the producer
//     declares the range it measures off the wire, and a consumer built a moment earlier declares
//     0:3:5:1 - so the consumer fails to negotiate the instant the producer's caps arrive
//     ("Preview error from intersrc_vN-appsrc: Internal data stream error.", or the same failure on
//     a recording's own leg). Caps fields are compared textually, so 0:3:5:1 and 1:3:5:1 do not
//     intersect even though the second is what the first was standing in for.
//
// Either way the format and geometry are pinned and the colour is left to whoever knows it.
static std::string make_video_caps_string_without_colorimetry()
{
  return fmt::format(
      "video/x-raw, format=YUY2, width={}, height={}, framerate={}/1, "
      "pixel-aspect-ratio=1/1, chroma-site=dv",
      osr::config.v_width,
      osr::config.v_height,
      osr::config.v_framerate);
}

// What a RANGE-AWARE leg of a chain declares. A measured camera declares the range its samples are
// in; one that has not been measured declares no range at all, so it can still negotiate with the
// producer that will declare one.
static std::string declared_video_caps_string(osr::RangeMode mode)
{
  const ColorRange range = encoded_range_for(mode);
  return range == ColorRange::Unspecified
      ? make_video_caps_string_without_colorimetry()
      : make_video_caps_string(range);
}

static std::string make_audio_caps_string()
{
  return fmt::format("audio/x-raw, format=F32LE, channels={}, rate={}",
                     osr::config.a_channels,
                     osr::config.a_sample_rate);
}

// The capture units differ in what they do with the source's quantisation range, and getting this
// wrong compresses or washes out the recording. Measured on this rig:
//
//   2b89:5518  UGREEN 65518  converts a full-range source to limited range in hardware, so the
//                            recorder must NOT convert again; that conversion is trimmed by the
//                            unit's own udev rule instead.
//   345f:2131  MS2131        passes the source's range through unchanged, so a full-range camera
//                            needs the full -> limited conversion applied here.
//
// An unmeasured device is left alone (passthrough) rather than guessed at. The identity is the USB
// vendor:product pair read from the capture node's sysfs entry: it is exact, and it is the same key
// the udev rules use.
static constexpr const char* kPassThroughCaptureUnitId = "345f:2131";

static std::string read_sysfs_line(const std::string& path)
{
  std::ifstream file(path);
  std::string value;
  if (!file.is_open())
    return {};
  std::getline(file, value);
  return value;
}

static std::string usb_id_of_capture_node(const std::string& device_path)
{
  const auto slash = device_path.find_last_of('/');
  const std::string node = slash == std::string::npos ? device_path : device_path.substr(slash + 1);
  if (node.empty())
    return {};
  // The video node hangs off the USB *interface*; idVendor/idProduct live on its parent, the device.
  const std::string base = "/sys/class/video4linux/" + node + "/device/..";
  const std::string vendor = read_sysfs_line(base + "/idVendor");
  const std::string product = read_sysfs_line(base + "/idProduct");
  if (vendor.empty() || product.empty())
    return {};
  return vendor + ":" + product;
}

// ---------------------------------------------------------------------------
// The JPEG XS stream's OWN range declaration
// ---------------------------------------------------------------------------
// Both camera nodes sign the video stream's quantisation range in TWO places on the wire: the
// JXS_video_descriptor in the PMT (video_full_range) and the 30-byte 'jxes' box that opens every
// access unit. GStreamer's tsdemux exposes NEITHER - its image/x-jxsc caps carry no range field -
// so a recorder that wants to declare the truth instead of assuming it has to read the marking off
// the wire itself. That is what this probe does, on the demuxer's SINK pad (before tsdemux), and
// the reading is done by the VENDORED parser (vendor/jxs/jxs_ts.c) - the same translation unit the
// OBS plugin ships - not by a second implementation of the same constants.
//
// Ordering, which is what makes this safe to consult when the branch caps are built: tsdemux cannot
// announce its pads until it has parsed the PMT, and the probe sees the PMT bytes BEFORE tsdemux
// does. So by the time jxs_pad_added() asks jxs_declared_range(), the PMT's declaration is in hand
// (and the per-frame box, if present, refines it per frame).
static jxs_range_probe_t* g_jxs_range_probe[osr::App::kMaxSources];
static std::atomic<int> g_jxs_declared_range[osr::App::kMaxSources];
static std::atomic<bool> g_jxs_declared_logged[osr::App::kMaxSources];

// JXS_RANGE_LIMITED is 0, so a zero-initialised array reports "measured limited" for every slot,
// whether or not its stream has ever been read - and range_mode_for_camera() then has an unmeasured
// JPEG XS camera declare bt709 while its producer declares the range the probe really read. Caps
// fields are compared textually, so those two cannot intersect and the preview dies not-negotiated
// at its first buffer ("Preview error from intersrc_vN-appsrc: Internal data stream error."). A slot
// therefore STARTS at JXS_RANGE_UNKNOWN - "not measured" must never be 0 - and is reset whenever the
// cameras move, since removing one shifts every later camera into a different slot.
static void reset_jxs_declared_ranges()
{
  for (size_t i = 0; i < osr::App::kMaxSources; ++i) {
    g_jxs_declared_range[i].store(JXS_RANGE_UNKNOWN);
    g_jxs_declared_logged[i].store(false);
  }
}

static int jxs_declared_range(size_t i)
{
  if (i >= osr::App::kMaxSources)
    return JXS_RANGE_UNKNOWN;
  return g_jxs_declared_range[i].load();
}

// The demux sink-pad probe: feed the vendored parser one chunk of transport stream and record what
// the stream declares. Cheap enough to run on every datagram - the parser reassembles codestreams,
// which is what it is for.
static GstPadProbeReturn jxs_range_sink_probe(GstPad*, GstPadProbeInfo* info, gpointer user_data)
{
  const size_t i = static_cast<size_t>(GPOINTER_TO_INT(user_data));
  if (i >= osr::App::kMaxSources)
    return GST_PAD_PROBE_OK;
  GstBuffer* buf = GST_PAD_PROBE_INFO_BUFFER(info);
  if (buf == nullptr)
    return GST_PAD_PROBE_OK;
  if (g_jxs_range_probe[i] == nullptr)
    g_jxs_range_probe[i] = jxs_range_probe_new();
  jxs_range_probe_t* probe = g_jxs_range_probe[i];
  if (probe == nullptr)
    return GST_PAD_PROBE_OK;

  GstMapInfo map;
  if (gst_buffer_map(buf, &map, GST_MAP_READ))
  {
    jxs_range_probe_feed(probe, map.data, map.size);
    gst_buffer_unmap(buf, &map);
  }

  const int measured = jxs_range_probe_full_range(probe);
  if (measured != JXS_RANGE_UNKNOWN)
  {
    g_jxs_declared_range[i].store(measured);
    if (!g_jxs_declared_logged[i].exchange(true))
    {
      uint8_t prim = 0, trans = 0, mat = 0;
      jxs_range_probe_colour(probe, &prim, &trans, &mat);
      // The per-leg declaration, in the recorder's own log, read from the wire rather than assumed:
      // this is the line that says what the take's colour/range tag will be and where it came from.
      logAppend(fmt::format(
          "CAM {}: JXS stream declares video_full_range={} (H.273 {}/{}/{}) from the {} -> this "
          "camera's range is MEASURED, not assumed",
          i, measured, prim, trans, mat, jxs_range_probe_source(probe)));
    }
  }
  return GST_PAD_PROBE_OK;
}

static osr::RangeMode range_mode_for_camera(size_t i)
{
  if (i < osr::config.range_modes.size())
    return osr::config.range_modes[i];

  // A JXS camera is not a GstDevice and has no capture-unit properties to ask - but the stream
  // declares its own range on the wire (see jxs_range_sink_probe), so there is nothing to guess:
  //   measured FULL    -> PassthroughFull    (full in, full out; declare 1:3:5:1)
  //   measured LIMITED -> PassthroughLimited (declare bt709)
  //   not measured yet -> Passthrough        (declare 0:3:5:1 - honest, nothing may claim limited)
  // The guard is not a nicety - JXS cameras occupy the slots AFTER video_sources, so without it
  // this read is out of bounds whenever no range override exists, and every Preview click visited
  // it (range_mode_for_camera <- build_preview_pipeline <- start_preview <- btn_preview_cb).
  if (is_jxs(i))
  {
    switch (jxs_declared_range(i))
    {
      case JXS_RANGE_FULL:
        return osr::RangeMode::PassthroughFull;
      case JXS_RANGE_LIMITED:
        return osr::RangeMode::PassthroughLimited;
      case JXS_RANGE_UNKNOWN:
      default:
        return osr::RangeMode::Passthrough;
    }
  }

  GstStructure* props = gst_device_get_properties(osr::config.video_sources[i]);
  if (props == nullptr)
    return osr::RangeMode::Passthrough;

  const gchar* path = gst_structure_get_string(props, OSR_VIDEO_SRC_PROP);
  if (path == nullptr)
    path = gst_structure_get_string(props, "device.name");
  const std::string id = path != nullptr ? usb_id_of_capture_node(path) : std::string{};
  gst_structure_free(props);

  if (id == kPassThroughCaptureUnitId)
    return osr::RangeMode::FullToLimited;
  return osr::RangeMode::Passthrough;
}

// The colorimetry the CAPTURE DEVICE itself states for the format this recorder opens it with, read
// off the GstDevice's own caps - or empty when the device states none.
//
// This is the only source of a device camera's colour, and it has to be: v4l2src validates the
// colorimetry named on its sink caps against the device's and REFUSES the stream when the two
// disagree.
//   ERROR: Device '/dev/video0' does not support bt709 colorimetry
//   gst_v4l2_object_set_format_full(): Device wants 2:4:5:1 colorimetry
//   streaming stopped, reason not-negotiated (-4)
// A value written into this file is therefore not a re-tagging of the samples - it stops the
// camera: no preview tile, and no recording either. Both HDMI units on this rig state a fixed pair
// whatever the source sends (MS2131 2:4:5:1, UGREEN 65518 2:4:7:1), and that statement is what this
// recorder carries through the chain.
static std::string device_advertised_colorimetry(size_t i)
{
  if (is_jxs(i) || i >= osr::config.video_sources.size() || !osr::config.video_sources[i])
    return {};

  GstCaps* caps = gst_device_get_caps(osr::config.video_sources[i]);
  if (caps == nullptr)
    return {};

  // A device states its colour per format, not per size: prefer the structure for the geometry this
  // recorder opens it with, and keep the format's statement for the case where that size is not
  // listed. The memory:DMABuf structures carry drm-format and no colour, so they are skipped.
  std::string format_statement;
  std::string geometry_statement;
  for (guint n = 0; n < gst_caps_get_size(caps); ++n)
  {
    const GstStructure* st = gst_caps_get_structure(caps, n);
    if (g_strcmp0(gst_structure_get_name(st), "video/x-raw") != 0)
      continue;
    if (g_strcmp0(gst_structure_get_string(st, "format"), "YUY2") != 0)
      continue;
    const gchar* colorimetry = gst_structure_get_string(st, "colorimetry");
    if (colorimetry == nullptr)
      continue;
    gint width = 0;
    gint height = 0;
    const gboolean geometry = gst_structure_get_int(st, "width", &width)
                              && gst_structure_get_int(st, "height", &height);
    if (geometry && width == osr::config.v_width && height == osr::config.v_height)
    {
      geometry_statement = colorimetry;
      break;
    }
    if (format_statement.empty())
      format_statement = colorimetry;
  }
  gst_caps_unref(caps);
  return geometry_statement.empty() ? format_statement : geometry_statement;
}

// The device's own colorimetry, carrying the range one leg of the chain has. The device supplies the
// coding (matrix, transfer, primaries) and this application's conversion element moves only the
// quantisation range, so a leg's declaration is the device's statement with that leg's range - and
// an unmeasured/unconverted leg is the device's statement as it stands.
static std::string device_colorimetry_for_range(size_t i, ColorRange range)
{
  const std::string advertised = device_advertised_colorimetry(i);
  if (advertised.empty() || range == ColorRange::Unspecified)
    return advertised;

  GstVideoColorimetry cinfo;
  if (!gst_video_colorimetry_from_string(&cinfo, advertised.c_str()))
    return advertised;
  cinfo.range = range == ColorRange::Full ? GST_VIDEO_COLOR_RANGE_0_255
                                          : GST_VIDEO_COLOR_RANGE_16_235;
  gchar* numeric = gst_video_colorimetry_to_string(&cinfo);
  const std::string converted = numeric != nullptr ? numeric : advertised;
  g_free(numeric);
  return converted;
}

// What a DEVICE camera's encoded leg declares. A chain with no conversion element carries the
// device's statement untouched; one with a conversion element carries the range that element
// produced, which is what its own src caps say (see range_declaration.h for the mapping).
static std::string device_declared_colorimetry(size_t i)
{
  return device_colorimetry_for_range(i, encoded_range_for(range_mode_for_camera(i)));
}

static std::string device_declared_caps_string(size_t i)
{
  const std::string colorimetry = device_declared_colorimetry(i);
  return colorimetry.empty()
      ? make_video_caps_string_without_colorimetry()
      : make_video_caps_string_with_colorimetry(colorimetry);
}

// The caps a camera's CONSUMER legs declare (the preview tile's first filter, the encode leg) - per
// camera kind, because the two kinds learn their colour differently: a device camera takes the
// device's own statement, a JPEG XS camera the range read off its stream.
static std::string declared_caps_for_camera(size_t i)
{
  if (is_jxs(i))
    return declared_video_caps_string(range_mode_for_camera(i));
  return device_declared_caps_string(i);
}

// The caps a preview tile declares for the compositor, which takes every tile in the limited range
// it outputs. A camera whose encoded leg is full therefore gets the tile's conversion element and
// the converted declaration after it - the same coding this application produced, in limited range.
static std::string tile_converted_caps_string(size_t i)
{
  if (is_jxs(i))
    return make_video_caps_string(ColorRange::Limited);
  const std::string colorimetry = device_colorimetry_for_range(i, ColorRange::Limited);
  return colorimetry.empty()
      ? make_video_caps_string_without_colorimetry()
      : make_video_caps_string_with_colorimetry(colorimetry);
}

// range_element_for() (the conversion element a mode needs, or none) lives in
// source/range_declaration.h with the mapping, so the test can pin that a MEASURED pass-through
// gains no element - the samples must reach the encoder untouched.

// Build the capture (producer) pipeline for ONE camera and return it. Each camera gets its own
// so that a camera which dies can be torn down and rebuilt in place without disturbing the
// others; the producer names carry the global camera index, which is how the shared preview
// pipeline and this camera's own encode pipeline find it over intersink/intersrc.
static GstElement* build_capture_pipeline(size_t i)
{
  GError* error = NULL;
  std::string pipelineStr = "";

  {
    auto video_device_props =
        gst_device_get_properties(osr::config.video_sources[i]);
    auto video_props_string = gst_structure_to_string(video_device_props);
    logAppend(video_props_string);
    g_free(video_props_string);
    // The identity key differs per OS (see OSR_*_SRC_PROP); fall back to the other spelling so a
    // monitor that reports only one of them still resolves the device.
    const gchar* video_device_path =
        gst_structure_get_string(video_device_props, OSR_VIDEO_SRC_PROP);
    if (!video_device_path)
      video_device_path = gst_structure_get_string(video_device_props, "device.name");
    // rtc: the timecode is anchored to the system clock (per frame, with a 250 ms drift resync),
    // so every camera's file in a take shares one TC origin - multicam sync works and a
    // restarted segment keeps its true position in the take. Metadata only: the stamper never
    // touches PTS, so muxing, A/V sync and the muxer's indexing are unaffected.
    // The colour of this leg is the DEVICE's, never this file's: v4l2src validates the colorimetry
    // named here against the device's own and refuses the stream when they differ ("Device
    // '/dev/video0' does not support bt709 colorimetry" -> not-negotiated), so a constant here does
    // not re-tag the samples, it stops the camera. Stating none leaves the device's own statement on
    // the wire, which is what the consumer legs then declare (see declared_caps_for_camera).
    pipelineStr += fmt::format(
        "{} ! {} ! timecodestamper auto-resync=false  set=always source=rtc ! ",
        video_source_element(osr::config.video_sources[i]),
        make_video_caps_string_without_colorimetry());

    // Only a unit that passes the source's range through gets the conversion; one that converts in
    // hardware must be left alone or the picture is compressed twice. A camera that needs nothing
    // gets no element at all, so its pipeline string is exactly what it was.
    if (const char* range_element = range_element_for(range_mode_for_camera(i)))
      pipelineStr += fmt::format("{} ! ", range_element);
    pipelineStr += fmt::format(
        "queue max-size-time={} ! intersink name=intersink_v "
        "producer-name=video{}sink  ",
        osr::config.max_buffer_time,
        i);
    gst_structure_free(video_device_props);

    if (source_audio(i)) {
      auto audio_device_props =
          gst_device_get_properties(osr::config.audio_sources[i]);
      auto audio_props_string = gst_structure_to_string(audio_device_props);
      logAppend(audio_props_string);
      g_free(audio_props_string);
      // audioconvert+audioresample: the IPC contract below is F32LE/48k/2ch, and a capture unit
      // hands its audio over as S16LE - the converter is what makes the contract true of every
      // device instead of only of the ones that happen to match it.
      pipelineStr += fmt::format("{} ! audioconvert ! audioresample ! {} ! ",
                                 audio_source_element(osr::config.audio_sources[i]),
                                 make_audio_caps_string());
      pipelineStr += fmt::format(
          "queue max-size-time={} ! intersink name=intersink_a "
          "producer-name=audio{}sink  ",
          osr::config.max_buffer_time,
          i);
      gst_structure_free(audio_device_props);
    } else if (i < osr::config.audio_sources.size() && osr::config.audio_sources[i]) {
      logAppend(fmt::format(
          "CAM {}: this audio device cannot be opened by a known source element - capturing "
          "video only", i));
    } else {
      logAppend(fmt::format("CAM {}: no audio device selected - capturing video only", i));
    }
  }

  logAppend(pipelineStr);

  auto pipeline = gst_parse_launch(pipelineStr.c_str(), &error);

  if (error) {
    log_pipeline_error("creating capture pipeline", error);
    if (pipeline) {
      gst_object_unref(pipeline);
      pipeline = nullptr;
    }
    return nullptr;
  }

  // Count frames off this camera's intersink pad: the drop accounting that the sidecar and the
  // fps watchdog depend on.
  GstElement* isink =
      gst_bin_get_by_name(GST_BIN(pipeline), "intersink_v");
  if (isink) {
    GstPad* pad = gst_element_get_static_pad(isink, "sink");
    if (pad) {
      gst_pad_add_probe(pad, GST_PAD_PROBE_TYPE_BUFFER, count_frame_cb,
                        GINT_TO_POINTER(static_cast<int>(i)), nullptr);
      // The same decoded video, tapped for the image-match walk. The probe copies nothing unless a
      // walk is running (g_match_capture_on), so an idle preview pays no more than one atomic load.
      gst_pad_add_probe(pad, GST_PAD_PROBE_TYPE_BUFFER, match_frame_cb,
                        GINT_TO_POINTER(static_cast<int>(i)), nullptr);
      gst_object_unref(pad);
    }
    gst_object_unref(isink);
  }
  // Report the colour the capture actually declares (see log_capture_caps_cb): the monitor's device
  // caps are not always able to say it, and the operator needs the number that ends up in the file.
  GstElement* capture_src = gst_bin_get_by_name(GST_BIN(pipeline), "capture_src");
  if (capture_src != nullptr)
  {
    GstPad* capture_pad = gst_element_get_static_pad(capture_src, "src");
    if (capture_pad != nullptr)
    {
      gst_pad_add_probe(capture_pad, GST_PAD_PROBE_TYPE_BUFFER, log_capture_caps_cb,
                        GINT_TO_POINTER(static_cast<int>(i)), nullptr);
      gst_object_unref(capture_pad);
    }
    gst_object_unref(capture_src);
  }
  return pipeline;
}

// ---------------------------------------------------------------------------
// JPEG XS cameras
// ---------------------------------------------------------------------------

// One branch of a tsdemux pad. tsdemux announces its pads only once the PMT has been
// parsed, so the branches below cannot be part of one pipeline string: with two
// elementary streams the launcher would attach the first compatible pad it finds and
// leave the other unlinked, which is GST_FLOW_NOT_LINKED and fatal for the pipeline.
// Every pad is linked here by its caps, and anything unrecognised goes to a fakesink
// so that an unlinked pad can never stall the demuxer.
//
// The branch chains are parse-bins with ghost pads because they must end at elements
// created at PIPELINE level: the intersinks are looked up by name from the pipeline
// (the preview and this camera's consumer both do that), and a bin-nested element
// would not be found.
static std::string jxs_video_branch_caps(size_t i)
{
  // Declared in the range the samples have, exactly as a device camera's chain does - but for a
  // network camera that range is READ FROM THE STREAM (jxs_range_sink_probe), not assumed from a
  // capture unit's USB id. This caps string is what the branch, the encode pipeline and the preview
  // tile all declare, and it is what the patched mxfmux reads at buffer time to write the file's
  // colour coding and range - so this is the one line that decides the recording's tag.
  const osr::RangeMode mode = range_mode_for_camera(i);
  const int measured = jxs_declared_range(i);
  const ColorRange range = encoded_range_for(mode);
  logAppend(fmt::format(
      "CAM {}: JXS declaration video_full_range={} -> unit leg colorimetry={}, encoded leg "
      "colorimetry={} (muxer will write this to the MXF)",
      i,
      measured,
      colorimetry_for(unit_range_for(mode)),
      colorimetry_for(range)));
  return make_video_caps_string(range);
}

// tsdemux passes the camera's own PES PTS straight through, and the nodes now stamp that
// PTS as the ABSOLUTE wall-clock time of capture. Rewrite it into THIS pipeline's
// running-time domain, so a JXS camera lands on the SAME timeline as the USB device
// cameras (whose do-timestamp sources already produce running-time) and so the rtc
// timecode names the instant the frame was really captured rather than "now".
//
// The PES field is 33 bits, so the value repeats every ~26.5 h; the signed difference
// against the current wall clock recovers which cycle it is - the same mapping the OBS
// plugin performs. A camera that is NOT stamping absolute time lands hours away, so
// anything further than 5 s from now is left untouched: this cannot regress a node that
// still uses a relative origin.
static const gint64 JXS_PTS_WRAP_NS = 95443717688LL;   // 2^33 ticks, expressed in ns

static gint64 osr_wall_ns()
{
  struct timespec t;
  clock_gettime(CLOCK_REALTIME, &t);
  return static_cast<gint64>(t.tv_sec) * 1000000000LL + t.tv_nsec;
}

static GstPadProbeReturn jxs_pts_normalise_cb(GstPad* pad, GstPadProbeInfo* info, gpointer)
{
  GstBuffer* buf = GST_PAD_PROBE_INFO_BUFFER(info);
  if (!buf || !GST_BUFFER_PTS_IS_VALID(buf))
    return GST_PAD_PROBE_OK;
  // The demux is a direct child of the capture pipeline (see jxs_pad_added).
  GstElement* pipeline = GST_ELEMENT(GST_OBJECT_PARENT(GST_OBJECT_PARENT(pad)));
  if (!pipeline)
    return GST_PAD_PROBE_OK;

  GstClock* clock = gst_element_get_clock(pipeline);
  const GstClockTime now_clock =
      clock ? gst_clock_get_time(clock) : gst_util_get_timestamp();
  if (clock)
    gst_object_unref(clock);
  const GstClockTime base = gst_element_get_base_time(pipeline);
  const GstClockTime running = now_clock > base ? now_clock - base : 0;

  const gint64 now_wall = osr_wall_ns();
  gint64 diff = static_cast<gint64>(GST_BUFFER_PTS(buf) % JXS_PTS_WRAP_NS) -
                (now_wall % JXS_PTS_WRAP_NS);
  if (diff > JXS_PTS_WRAP_NS / 2)
    diff -= JXS_PTS_WRAP_NS;
  else if (diff < -JXS_PTS_WRAP_NS / 2)
    diff += JXS_PTS_WRAP_NS;

  if (diff > 5 * GST_SECOND || diff < -5 * GST_SECOND)
    return GST_PAD_PROBE_OK;   // not an absolute stamp: leave the buffer alone

  const gint64 rt = static_cast<gint64>(running) + diff;
  GST_BUFFER_PTS(buf) = rt > 0 ? static_cast<GstClockTime>(rt) : 0;
  return GST_PAD_PROBE_OK;
}

static void jxs_pad_added(GstElement* demux, GstPad* pad, gpointer user)
{
  const size_t i = static_cast<size_t>(GPOINTER_TO_INT(user));
  const osr::JxsCamera& cam = jxs_of(i);
  // The pipeline that owns this demux: ONE level up. demux is a direct child of
  // the capture pipeline (it is named in the gst_parse_launch string), so its
  // parent IS the GstBin the branches must be added to. Two levels up is NULL -
  // which made every gst_bin_add/get_by_name here assert and the video branch
  // never reach intersink_v: the preview tile stayed black while the stream
  // kept flowing (the journal showed the GST_IS_BIN assertions on Preview).
  GstElement* pipeline = GST_ELEMENT(GST_OBJECT_PARENT(demux));
  GError* error = nullptr;

  GstCaps* caps = gst_pad_get_current_caps(pad);
  if (!caps)
    caps = gst_pad_query_caps(pad, nullptr);
  const GstStructure* st = caps ? gst_caps_get_structure(caps, 0) : nullptr;
  const gchar* name = st ? gst_structure_get_name(st) : "";

  if (g_strcmp0(name, "image/x-jxsc") == 0) {
    // Put the camera's absolute capture time onto this pipeline's timeline, before the
    // branch is linked (see jxs_pts_normalise_cb). Guarded so a re-announced pad cannot
    // stack a second probe on the same pad.
    if (!g_object_get_data(G_OBJECT(pad), "osr-pts-normalised")) {
      gst_pad_add_probe(pad, GST_PAD_PROBE_TYPE_BUFFER, jxs_pts_normalise_cb,
                        nullptr, nullptr);
      g_object_set_data(G_OBJECT(pad), "osr-pts-normalised", GINT_TO_POINTER(1));
    }
  }

  GstElement* branch = nullptr;
  if (g_strcmp0(name, "image/x-jxsc") == 0) {
    // The capsfilter is mandatory: tsdemux negotiates JPEG XS but its output carries
    // no `sampling` field and svtjpegxsdec requires one (measured; tsdemux ! svtjpegxsdec
    // fails not-negotiated without it).
    std::string chain;
    if (cam.mode == osr::JxsRecordMode::ToMkv) {
      const std::string mkv_file = segment_path(i);
      chain = fmt::format(
          "queue max-size-buffers=8 ! capsfilter caps=\"image/x-jxsc,sampling=(string)YCbCr-4:2:0\" "
          "! svtjpegxsdec ! tee name=jvt "
          "jvt. ! queue max-size-buffers=64 ! avenc_ffv1 ! matroskamux name=jxsmkv "
          "! filesink location={} sync=false "
          "jvt. ! queue max-size-time={} ! videoconvert "
          "! capsfilter caps=\"{}\""
          " ! timecodestamper auto-resync=false set=always source=rtc "
          "! queue",
          escape(mkv_file.c_str(), {' '}, '\\'), osr::config.max_buffer_time,
          jxs_video_branch_caps(i));
    } else {
      chain = fmt::format(
          "queue max-size-buffers=8 ! capsfilter caps=\"image/x-jxsc,sampling=(string)YCbCr-4:2:0\" "
          "! svtjpegxsdec ! videoconvert "
          "! capsfilter caps=\"{}\""
          " ! timecodestamper auto-resync=false set=always source=rtc "
          "! queue max-size-time={}",
          jxs_video_branch_caps(i), osr::config.max_buffer_time);
    }
    // tsdemux can re-announce a pad (measured: the phone re-fires pad-added for its
    // video ES ~1 s after the first; rsinter re-registers the producer). Rebuilding
    // the branch would gst_bin_add a second element named "jxsvideo", which fails and
    // FREES the new branch - the code then linked a dangling pointer and the tile
    // died with "cannot link the video branch". If the branch already exists, this is
    // the same camera's pad re-announced: nothing to do, leave the existing link.
    if (gst_bin_get_by_name(GST_BIN(pipeline), "jxsvideo")) {
      gst_caps_unref(caps);
      return;
    }
    branch = gst_parse_bin_from_description(chain.c_str(), TRUE, &error);
    if (!branch) {
      logAppend(fmt::format("CAM {}: cannot build the video branch: {}",
                            i, error ? error->message : "unknown"));
      if (error) g_clear_error(&error);
      gst_caps_unref(caps);
      return;
    }
    gst_element_set_name(branch, "jxsvideo");
    gst_bin_add(GST_BIN(pipeline), branch);
    // The capture pipeline is already PLAYING when pad-added fires (udpsrc is
    // running and tsdemux just parsed the PMT). A branch added to a PLAYING bin
    // starts in NULL and never streams: tsdemux pushes one buffer, then stalls
    // (measured: 1 frame then silence without the sync, full rate with it). This
    // was the checkerboard - the tile was wired but the producer never delivered.
    gst_element_sync_state_with_parent(branch);
    GstElement* isink = gst_bin_get_by_name(GST_BIN(pipeline), "intersink_v");
    if (!isink) {
      // Reached only if the pipeline was built without its intersink - a bug here,
      // not a stream problem, so say so rather than dropping frames silently.
      logAppend(fmt::format("CAM {}: no intersink_v in the pipeline", i));
      gst_caps_unref(caps);
      return;
    }
    (void)cam;
    if (!gst_element_link(branch, isink))
      logAppend(fmt::format("CAM {}: cannot link the video branch", i));
    gst_object_unref(isink);
    GstPad* sinkpad = gst_element_get_static_pad(branch, "sink");
    if (sinkpad) {
      gst_pad_link(pad, sinkpad);
      gst_object_unref(sinkpad);
    }
  } else if (g_strcmp0(name, "audio/x-smpte-302m") == 0 && cam.audio) {
    // The node's audio arrives as SMPTE ST 302M (AES3-packed PCM) and is decoded once,
    // here: no codec anywhere on the link or in the file. F32LE into the IPC hop, which
    // is what a device camera's audio also uses.
    std::string chain = fmt::format(
        "queue max-size-buffers=8 ! capsfilter caps=\"audio/x-smpte-302m\" ! avdec_s302m "
        "! audioconvert ! audioresample ! capsfilter caps=\"audio/x-raw,format=F32LE,channels=2,rate=48000\" "
        "! queue max-size-time={}",
        osr::config.max_buffer_time);
    branch = gst_parse_bin_from_description(chain.c_str(), TRUE, &error);
    if (!branch) {
      logAppend(fmt::format("CAM {}: cannot build the audio branch: {}",
                            i, error ? error->message : "unknown"));
      if (error) g_clear_error(&error);
      gst_caps_unref(caps);
      return;
    }
    gst_bin_add(GST_BIN(pipeline), branch);
    // Same as the video branch: the capture pipeline is already PLAYING when the
    // audio pad appears; an unsynced branch never streams (one buffer then stall).
    gst_element_sync_state_with_parent(branch);
    if (cam.mode == osr::JxsRecordMode::ToMkv) {
      // The file this mode writes is the one the VIDEO branch opened, so the audio is
      // decoded to S24LE - the conventional broadcast depth - and linked into that very
      // muxer. matroskamux creates audio pads on request.
      GstElement* vb = gst_bin_get_by_name(GST_BIN(pipeline), "jxsvideo");
      GstElement* mkv = vb ? gst_bin_get_by_name(GST_BIN(vb), "jxsmkv") : nullptr;
      if (mkv) {
        std::string audio_chain = fmt::format(
            "queue max-size-buffers=8 ! capsfilter caps=\"audio/x-smpte-302m\" ! avdec_s302m "
            "! audioconvert ! audioresample "
            "! capsfilter caps=\"audio/x-raw,format=S24LE,channels=2,rate=48000\" ! queue",
            osr::config.max_buffer_time);
        GstElement* abranch = gst_parse_bin_from_description(audio_chain.c_str(), TRUE, &error);
        if (abranch) {
          gst_bin_add(GST_BIN(pipeline), abranch);
          gst_element_sync_state_with_parent(abranch);
          GstPad* asink = gst_element_get_static_pad(abranch, "sink");
          GstPad* apad = gst_element_request_pad_simple(mkv, "audio_%u");
          GstPad* asrc = gst_element_get_static_pad(abranch, "src");
          if (asink && apad && asrc) {
            gst_pad_link(pad, asink);
            if (!gst_pad_link(asrc, apad))
              logAppend(fmt::format("CAM {}: cannot link the audio into the MKV", i));
          }
          if (asink) gst_object_unref(asink);
          if (apad) gst_object_unref(apad);
          if (asrc) gst_object_unref(asrc);
        } else if (error) {
          g_clear_error(&error);
        }
        gst_object_unref(mkv);
      }
      if (vb) gst_object_unref(vb);
      if (caps) gst_caps_unref(caps);
      return;
    }
    GstElement* isink = gst_bin_get_by_name(GST_BIN(pipeline), "intersink_a");
    if (isink) {
      if (!gst_element_link(branch, isink))
        logAppend(fmt::format("CAM {}: cannot link the audio branch", i));
      gst_object_unref(isink);
    } else {
      logAppend(fmt::format("CAM {}: no intersink_a to receive the audio", i));
    }
    GstPad* sinkpad = gst_element_get_static_pad(branch, "sink");
    if (sinkpad) {
      gst_pad_link(pad, sinkpad);
      gst_object_unref(sinkpad);
    }
  } else {
    // An elementary stream this app has no use for: absorb it, so the pad is linked
    // and the demuxer keeps making progress.
    branch = gst_parse_bin_from_description("queue ! fakesink sync=false", TRUE, &error);
    if (branch) {
      gst_bin_add(GST_BIN(pipeline), branch);
      gst_element_sync_state_with_parent(branch);
      GstPad* sinkpad = gst_element_get_static_pad(branch, "sink");
      if (sinkpad) { gst_pad_link(pad, sinkpad); gst_object_unref(sinkpad); }
    } else if (error) {
      g_clear_error(&error);
    }
  }
  if (caps) gst_caps_unref(caps);
}

// Build the capture (producer) pipeline for ONE JPEG XS camera. See the brief's
// decisions 3 and 4: the stream arrives as a transport stream over UDP; the file leg
// of the passthrough mode taps the tee BEFORE tsdemux (that is what makes the file the
// camera's own bytes), and the decoded picture feeds intersink_v by producer name
// exactly as a device camera does, so the preview and the consumer are unchanged.
static GstElement* build_jxs_capture_pipeline(size_t i)
{
  const osr::JxsCamera& cam = jxs_of(i);
  GError* error = nullptr;
  std::string pipelineStr;

  // buffer-size is deliberate: the stream runs at ~174 Mbit/s and udpsrc's default
  // buffer drops packets under scheduler jitter. In multicast the camera arrives on
  // its own GROUP at the shared port. The receive socket is NOT built by udpsrc:
  // make_multicast_socket() creates it with IP_MULTICAST_ALL=0 (Linux's default of
  // 1 makes every socket on the port receive EVERY group's datagrams - measured:
  // two sockets on port 5000 each saw both cameras - so the second camera's TS
  // interleaves into the first demux and the decoder dies with "Could not decode
  // stream"). The socket is injected via the "socket" property after parse
  // (classic udpsrc has no IP_MULTICAST_ALL property), so the string carries no
  // multicast-group/multicast-source - that join happens on the raw socket.
  if (cam.transport == osr::JxsTransport::Multicast) {
    pipelineStr += fmt::format(
        "udpsrc name=jxsudpsrc buffer-size=4194304 ! "
        "queue max-size-buffers=8 ! ");
  } else {
    pipelineStr += fmt::format(
        "udpsrc port={} buffer-size=4194304 ! queue max-size-buffers=8 ! ",
        cam.port);
  }
  if (cam.mode == osr::JxsRecordMode::Passthrough) {
    // Nothing to mux: the codestream is already a transport stream. MPEG-TS has to be
    // the container here - mpegtsmux is the only muxer that advertises image/x-jxsc,
    // and Matroska cannot carry JPEG XS at all.
    const std::string take_file = segment_path(i);
    pipelineStr += fmt::format(
        "tee name=jxt "
        "jxt. ! queue max-size-buffers=64 ! filesink location={} sync=false "
        "jxt. ! queue max-size-buffers=8 ! ",
        escape(take_file.c_str(), {' '}, '\\'));
  }
  pipelineStr += "tsdemux name=demux ";
  pipelineStr += fmt::format(
      "intersink name=intersink_v producer-name=video{}sink ", i);
  if (source_audio(i))
    pipelineStr += fmt::format("intersink name=intersink_a producer-name=audio{}sink ", i);

  logAppend(pipelineStr);
  GstElement* pipeline = gst_parse_launch(pipelineStr.c_str(), &error);
  if (error) {
    log_pipeline_error("creating JXS capture pipeline", error);
    if (pipeline) { gst_object_unref(pipeline); pipeline = nullptr; }
    return nullptr;
  }

  // Multicast: hand udpsrc a pre-built socket (IP_MULTICAST_ALL=0 + group joined)
  // so each camera's demux only ever sees its own group's TS on the shared port.
  // The element takes ownership and closes it on teardown (close-socket=TRUE).
  // Fall back to udpsrc's own socket if creation fails (it worked for one camera).
  if (cam.transport == osr::JxsTransport::Multicast) {
    const int mfd = make_multicast_socket(cam.group, cam.port);
    if (mfd >= 0) {
      GSocket* gs = g_socket_new_from_fd(mfd, nullptr);
      GstElement* udp = gst_bin_get_by_name(GST_BIN(pipeline), "jxsudpsrc");
      if (udp && gs) {
        g_object_set(udp, "socket", gs, "close-socket", TRUE, nullptr);
      } else if (gs) {
        g_object_unref(gs);
      } else {
        close(mfd);
      }
      if (udp)
        gst_object_unref(udp);
    } else {
      logAppend("JXS camera " + cam.host + ": multicast socket failed, "
                "falling back to plain udpsrc");
    }
  }

  GstElement* demux = gst_bin_get_by_name(GST_BIN(pipeline), "demux");
  if (demux) {
    g_signal_connect(demux, "pad-added", G_CALLBACK(jxs_pad_added),
                     GINT_TO_POINTER(static_cast<int>(i)));
    // Read the stream's own range marking before tsdemux does (jxs_range_sink_probe): the probe
    // must sit on the SINK pad, the one that receives the TS bytes, and it is attached before the
    // pipeline goes to PLAYING, so no PMT is ever missed. The value is what jxs_video_branch_caps()
    // declares when tsdemux announces the video pad (which it cannot do until it has parsed that
    // same PMT), so the branch caps and the recorded tag are decided from the declaration.
    GstPad* dsink = gst_element_get_static_pad(demux, "sink");
    if (dsink) {
      gst_pad_add_probe(dsink, GST_PAD_PROBE_TYPE_BUFFER, jxs_range_sink_probe,
                        GINT_TO_POINTER(static_cast<int>(i)), nullptr);
      gst_object_unref(dsink);
    }
    gst_object_unref(demux);
  }

  // Count frames off this camera's intersink pad, exactly as a device camera does: the
  // sidecar and the fps watchdog read these counters.
  GstElement* isink = gst_bin_get_by_name(GST_BIN(pipeline), "intersink_v");
  if (isink) {
    GstPad* s = gst_element_get_static_pad(isink, "sink");
    if (s) {
      gst_pad_add_probe(s, GST_PAD_PROBE_TYPE_BUFFER, count_frame_cb,
                        GINT_TO_POINTER(static_cast<int>(i)), nullptr);
      // Same decoded video as the counter above, tapped for the image-match walk (idle => no copy).
      gst_pad_add_probe(s, GST_PAD_PROBE_TYPE_BUFFER, match_frame_cb,
                        GINT_TO_POINTER(static_cast<int>(i)), nullptr);
      gst_object_unref(s);
    }
    gst_object_unref(isink);
  }
  return pipeline;
}

static void build_preview_pipeline()
{
  GError* error = NULL;
  std::string pipelineStr = "";

  // A tile's caps filter has to declare the same range the producer will declare, or the two cannot
  // intersect. The producer's branch caps are decided when tsdemux announces its video pad, from the
  // range MEASURED off the wire, and intersrc then delivers exactly those caps - so a tile built
  // from a different range fails to negotiate as soon as the preview starts:
  //   Preview error from intersrc_vN-appsrc: Internal data stream error.
  // The measurement needs the camera's transport stream to arrive, which happens a moment after
  // start_session() starts the camera thread that builds the capture pipeline. On the first Preview
  // after cameras were added the measurement has therefore not landed yet, so give it a bounded
  // moment here and build the tile from the same value the producer will use. A camera that never
  // announces itself is left declared unknown - what it was before - and shows up as a dead tile in
  // the health indicator rather than as a preview that will not start at all.
  constexpr gint64 JXS_RANGE_WAIT_US = 4000 * 1000;
  for (size_t i = 0; i < camera_count(); ++i) {
    if (!is_jxs(i) || jxs_declared_range(i) != JXS_RANGE_UNKNOWN)
      continue;
    const gint64 deadline = g_get_monotonic_time() + JXS_RANGE_WAIT_US;
    while (jxs_declared_range(i) == JXS_RANGE_UNKNOWN && g_get_monotonic_time() < deadline)
      g_usleep(100 * 1000);
    logAppend(fmt::format(
        "CAM {}: JXS range wait -> {}", i,
        jxs_declared_range(i) == JXS_RANGE_UNKNOWN
            ? "still not announced; its tile stays declared range-unknown"
            : "measured, tile built from the declaration"));
  }

  pipelineStr += "compositor name=compositor  ! ";
  {
    std::string labels;
    for (size_t i = 0; i < camera_count(); ++i)
      labels += fmt::format("{}{}=[{}]", i ? " " : "", i, camera_label(i));
    logAppend(fmt::format("Preview: building {} tile(s): {}", camera_count(), labels));
  }
  // The compositor output is limited: every tile below either arrives limited already or is
  // converted into it, so this declaration is true of the samples, not just of the intent.
  pipelineStr += make_video_caps_string(ColorRange::Limited);
  pipelineStr += " ! videoconvert ! queue max-size-buffers=3 ! autovideosink ";

  // The audio half of the preview exists only when at least one source has audio. An audiomixer
  // with no inputs never produces a buffer, so adding it unconditionally would leave a video-only
  // preview waiting on a mixer that can never preroll.
  bool any_audio = false;
  for (size_t i = 0; i < camera_count(); ++i) {
    if (source_audio(i))
      any_audio = true;
  }
  if (any_audio)
    pipelineStr +=
        "audiomixer name=mix ! audioconvert ! queue max-size-buffers=3 ! "
        "autoaudiosink  ";

  for (size_t i = 0; i < camera_count(); ++i) {
    // A tile must reach the compositor in the range the compositor's output declares. A camera
    // whose producer converted to limited already does; one in limited-to-full hands over
    // full-range samples and gets the converter here, per tile, so the preview does not show it
    // brighter than it will be recorded. What a tile declares is what its chain actually carries
    // (see declared_caps_for_camera): a device camera declares the colour its device states, a JPEG
    // XS camera the range measured off its stream, and neither invents one - a declaration the
    // producer does not make cannot negotiate, and that is what stopped HDMI sources previewing.
    const osr::RangeMode mode = range_mode_for_camera(i);
    pipelineStr += fmt::format(
        "intersrc name=intersrc_v{} producer-name=video{}sink ! {} ! ",
        i,
        i,
        declared_caps_for_camera(i));
    if (encoded_range_for(mode) == ColorRange::Full) {
      // Full-range samples arriving into a limited preview (either a limited-to-full unit camera or
      // a JPEG XS node that signed full range): convert this tile, and declare the converted
      // result, so the compositor's limited output is true of every tile it contains.
      pipelineStr += fmt::format("osrrange mode=full-to-limited ! {} ! ",
                                 tile_converted_caps_string(i));
    }
    // One short line per tile saying what it declares and whether it carries the conversion. Every
    // failure in this area is a caps-negotiation failure, and this is the decision that causes it -
    // the raw caps string is long enough to be clipped in the log pane, so state it in words.
    if (is_jxs(i)) {
      const ColorRange tile_range = encoded_range_for(mode);
      logAppend(fmt::format(
          "CAM {}: preview tile: {} -> {}",
          i,
          tile_range == ColorRange::Unspecified
              ? std::string("range NOT measured, so no range is declared")
              : fmt::format("range measured, declaring {}", colorimetry_for(tile_range)),
          tile_range == ColorRange::Full ? "osrrange mode=full-to-limited, declaring bt709"
                                         : std::string("no conversion element")));
    } else {
      const std::string colorimetry = device_declared_colorimetry(i);
      logAppend(fmt::format(
          "CAM {}: preview tile: declaring the capture device's own colorimetry{} -> {}",
          i,
          colorimetry.empty() ? std::string(", of which the device states none")
                              : fmt::format(" ({})", colorimetry),
          encoded_range_for(mode) == ColorRange::Full
              ? "osrrange mode=full-to-limited, declaring it in limited range"
              : std::string("no conversion element")));
    }
    pipelineStr += fmt::format("queue max-size-time={} ! ", osr::config.max_buffer_time);
    // Per-tile health indicator. health_tick_cb() rewrites text/color from the real frame
    // counters twice a second: green = this camera's chain is moving frames, red = it is not.
    pipelineStr += fmt::format(
        "timeoverlay time-mode=time-code ! "
        "textoverlay name=health{} text=\"CAM {}\" valignment=top halignment=right "
        "xpad=14 ypad=12 font-desc=\"Sans Bold 20\" shaded-background=true ! "
        "queue max-size-buffers=3 ! compositor. ",
        i, i);

    if (source_audio(i)) {
      pipelineStr += fmt::format(
          "intersrc name=intersrc_a{} producer-name=audio{}sink ! {} ! queue max-size-time={} ! ",
          i,
          i,
          make_audio_caps_string(),
          osr::config.max_buffer_time);
      pipelineStr += "mix. ";
    }
  }

  auto pipeline = gst_parse_launch(pipelineStr.c_str(), &error);
  logAppend(pipelineStr);

  int row = 0, col = 0;
  const int rows =
      (camera_count() + osr::config.preview_cols - 1)
      / osr::config.preview_cols;
  const int tile_width = osr::config.v_width / osr::config.preview_cols;
  const int tile_height = osr::config.v_height / rows;

  for (size_t i = 0; i < camera_count(); ++i) {
    GstElement* intersrc_v = gst_bin_get_by_name(
        GST_BIN(pipeline), fmt::format("intersrc_v{}", i).c_str());
    GstElement* internal_appsrc_v = gst_bin_get_by_name(
        GST_BIN(intersrc_v), fmt::format("intersrc_v{}-appsrc", i).c_str());
    g_object_set(internal_appsrc_v, "do-timestamp", FALSE, NULL);
    g_object_set(internal_appsrc_v, "is-live", TRUE, NULL);
    gst_object_unref(intersrc_v);
    gst_object_unref(internal_appsrc_v);

    if (source_audio(i)) {
      GstElement* intersrc_a = gst_bin_get_by_name(
          GST_BIN(pipeline), fmt::format("intersrc_a{}", i).c_str());
      GstElement* internal_appsrc_a = gst_bin_get_by_name(
          GST_BIN(intersrc_a), fmt::format("intersrc_a{}-appsrc", i).c_str());
      g_object_set(internal_appsrc_a, "do-timestamp", FALSE, NULL);
      g_object_set(internal_appsrc_a, "is-live", TRUE, NULL);
      gst_object_unref(intersrc_a);
      gst_object_unref(internal_appsrc_a);
    }

    auto compositor = gst_bin_get_by_name(GST_BIN(pipeline), "compositor");

    auto compositor_sink_pad = gst_element_get_static_pad(
        compositor, fmt::format("sink_{}", i).c_str());

    // Set pad properties for tiling position and size
    int xpos = col * tile_width;
    int ypos = row * tile_height;
    g_object_set(compositor_sink_pad,
                 "xpos",
                 xpos,
                 "ypos",
                 ypos,
                 "width",
                 tile_width,
                 "height",
                 tile_height,
                 "sizing-policy",
                 1,  // 1 = keep-aspect-ratio
                 NULL);

    // Update grid position
    col++;
    if (col >= osr::config.preview_cols) {
      col = 0;
      row++;
    }
  }

  if (error) {
    log_pipeline_error("creating preview pipeline", error);
  } else {
    osr::app.preview_pipeline = pipeline;
  }
}

// Per-camera output file for the camera's current segment. The first segment of a take keeps the
// name the operator typed; later ones are suffixed _seg2, _seg3 ... so a restart never
// overwrites footage that is already on disk.
static std::string segment_path(size_t i)
{
  const int seg = osr::app.source[i].segment.load();
  std::string base = osr::config.filenames[i];
  if (seg > 1)
    base += fmt::format("_seg{}", seg);
  // A JXS camera's own mode decides the container, not the take's codec: passthrough
  // records the camera's transport stream verbatim and ToMkv writes lossless FFV1 in
  // Matroska. Both are per camera because they describe that camera's link.
  if (is_jxs(i)) {
    if (jxs_of(i).mode == osr::JxsRecordMode::Passthrough)
      return base + ".ts";
    if (jxs_of(i).mode == osr::JxsRecordMode::ToMkv)
      return base + ".mkv";
  }
  return base + osr::get_filename_extension(osr::config.codec);
}

// Build the encode (consumer) pipeline for ONE camera and return it: one encoder chain, one
// muxer, one file. Owning a pipeline per camera is what lets a failed camera be rebuilt without
// interrupting the others.
static GstElement* build_encode_pipeline(size_t i)
{
  GError* error = NULL;
  std::string pipelineStr = "";

  pipelineStr += fmt::format(
      "intersrc name=intersrc_v producer-name=video{}sink ! {} ! ",
      i,
      declared_caps_for_camera(i));
  // threads=auto is the only multi-thread setting gst-libav accepts (the property's enum refuses
  // specific counts); single-threaded cost 2-3.5x throughput in measurements on both rigs.
  // Frame-threaded output stays in order through the muxer (verified end-to-end).
  pipelineStr += fmt::format(
      "queue max-size-buffers=3 ! videoconvert ! {} name=encoder_v threads=auto ! queue max-size-time={} ! "
      "{} name=mux {}! queue max-size-buffers=3 ! filesink sync=false "
      "async=false location={} ",
      osr::get_codec_element_name(osr::config.codec),
      osr::config.max_buffer_time,
      osr::get_muxer_name(osr::config.codec),
      osr::get_muxer_properties(osr::config.codec, osr::config.quality) + " ",
      segment_path(i));

  if (source_audio(i)) {
    pipelineStr += fmt::format(
        "intersrc name=intersrc_a producer-name=audio{}sink ! {} ! ",
        i,
        make_audio_caps_string());

    // Pin the PCM depth that reaches the muxer. audioconvert would otherwise leave the choice
    // to the muxer, and mxfmux picks 32-bit from our F32LE source. S24LE is the conventional
    // broadcast mezzanine depth and the most interoperable one for NLE ingest. The muxer accepts
    // S16LE/S24LE/S32LE and writes back whichever depth it is handed, so this is a choice, not a
    // constraint.
    pipelineStr += fmt::format(
        "queue max-size-time={} ! audioconvert ! audioresample ! "
        "audio/x-raw,format=S24LE ! queue max-size-buffers=3 ! mux. ",
        osr::config.max_buffer_time);
  } else {
    // Video-only source: the muxer gets a single video pad. Both muxers this app uses handle
    // that - mxfmux writes a video-only MXF (measured on gst 1.28.6: 1920x1080 DNxHR, 3.0 s,
    // one video stream, valid container) and qtmux writes a video-only MOV.
    logAppend(fmt::format("CAM {}: no audio device selected - muxing video only", i));
  }

  logAppend(pipelineStr);
  auto pipeline = gst_parse_launch(pipelineStr.c_str(), &error);

  if (error) {
    g_print("Error creating encode pipeline: %s\n", error->message);
    g_clear_error(&error);
    if (pipeline) {
      gst_object_unref(pipeline);
      pipeline = nullptr;
    }
    return nullptr;
  }

  // Time comes from the producer side across the IPC hop; the consumer's own appsrc must never
  // timestamp, or the file would not line up with the other cameras.
  GstElement* intersrc_v = gst_bin_get_by_name(
      GST_BIN(pipeline), "intersrc_v");
  GstElement* internal_appsrc_v = gst_bin_get_by_name(
      GST_BIN(intersrc_v), "intersrc_v-appsrc");
  g_object_set(internal_appsrc_v, "do-timestamp", FALSE, NULL);
  g_object_set(internal_appsrc_v, "is-live", TRUE, NULL);
  gst_object_unref(intersrc_v);
  gst_object_unref(internal_appsrc_v);

  if (source_audio(i)) {
    GstElement* intersrc_a = gst_bin_get_by_name(
        GST_BIN(pipeline), "intersrc_a");
    GstElement* internal_appsrc_a = gst_bin_get_by_name(
        GST_BIN(intersrc_a), "intersrc_a-appsrc");
    g_object_set(internal_appsrc_a, "do-timestamp", FALSE, NULL);
    g_object_set(internal_appsrc_a, "is-live", TRUE, NULL);
    gst_object_unref(intersrc_a);
    gst_object_unref(internal_appsrc_a);
  }

  GstElement* encoder_v = gst_bin_get_by_name(
      GST_BIN(pipeline), "encoder_v");
  if (encoder_v) {
    g_object_set(
        encoder_v,
        osr::get_quality_name(osr::config.codec),
        osr::get_setting_for_quality(osr::config.quality, osr::config.codec),
        NULL);
    // Count encoded frames for the preview's health indicator: a frozen counter while a take is
    // open means this camera's file side is not receiving anything.
    GstPad* enc_src_pad = gst_element_get_static_pad(encoder_v, "src");
    if (enc_src_pad) {
      gst_pad_add_probe(enc_src_pad, GST_PAD_PROBE_TYPE_BUFFER, count_encode_cb,
                        GINT_TO_POINTER(static_cast<int>(i)), nullptr);
      gst_object_unref(enc_src_pad);
    }
    gst_object_unref(encoder_v);
  }

  // NOTE: no per-camera timestamp offset is applied to the encoder chain.
  // A "tsoffset" element used to be looked up here, but it was never created anywhere in the
  // pipeline string, so this called g_object_set(NULL, ...) - a GLib critical on every
  // recording - and no offset was ever applied. Alignment now comes from the file timecode
  // seeded out of videotimecodemeta by the patched mxfmux (tools/build-patched-gstmxf-*.sh),
  // not from a buffer ts-offset. If a per-camera offset is ever wanted, add the element to the
  // pipeline string above AND check it exists before setting it.

  return pipeline;
}

// ---------------------------------------------------------------------------
// Per-camera run loop
// ---------------------------------------------------------------------------

// The clock every pipeline in a take shares. It is a GstNetClientClock slaved to the video
// LAN's net-time provider (the router's GstNetTimeProvider), so a frame's PTS means the same
// instant on every node and recordings from different runs and machines are directly comparable.
//
// The provider serves the SAME CLOCK_REALTIME (UTC epoch) the router uses as PTP grandmaster and
// NTP server, and the camera nodes slave to it the same way - so recorder and cameras share one
// timeline. GstNetClientClock is self-slaving: gst_clock_get_time() returns the corrected
// provider time. Unlike GstPtpClock it needs no privileged UDP 319/320 and no gst-ptp-helper
// file capabilities, so it locks even where ptp4l already owns the PTP ports (the normal case
// here) - the reason this replaced GstPtpClock.
//
// The provider is 192.168.1.1:5637 by default. Override with OSR_NETTIME=<host>[:<port>];
// OSR_NETTIME=off disables it and drops straight to the CLOCK_REALTIME fallback.
//
// The fallback is CLOCK_REALTIME, NOT MONOTONIC. On a host whose CLOCK_REALTIME is PTP-
// disciplined (the normal case here: ptp4l already runs) REALTIME *is* the LAN timeline, so
// timestamps stay comparable across machines. MONOTONIC is time-since-boot - a DIFFERENT
// timeline that looks perfectly healthy while silently destroying cross-machine alignment. That
// was the previous behaviour and it is the bug this fixes. Set OSR_NETTIME=off to force the
// fallback deliberately.
static GstClock* acquire_sync_clock()
{
  const gchar* want = g_getenv("OSR_NETTIME");
  if (!(want && g_strcmp0(want, "off") == 0)) {
    gchar* host = g_strdup((want && *want) ? want : "192.168.1.1");
    gint port = 5637;
    gchar* colon = g_strrstr(host, ":");
    if (colon) {
      *colon = '\0';
      const gint p = static_cast<gint>(g_ascii_strtoll(colon + 1, nullptr, 10));
      if (p > 0 && p <= 65535)
        port = p;
    }
    GstClock* net = gst_net_client_clock_new("osr-nettime", host, port, 0);
    if (net && gst_clock_wait_for_sync(net, 5 * GST_SECOND)) {
      logAppend(fmt::format("Clock: net-time locked to {}:{} - every pipeline shares the LAN "
                            "(UTC) timeline.", host, port));
      g_free(host);
      return net;
    }
    logAppend(fmt::format("Clock: no net-time provider locked at {}:{} (is the LAN provider "
                          "reachable?) - falling back to CLOCK_REALTIME.", host, port));
    if (net)
      gst_object_unref(net);
    g_free(host);
  } else {
    logAppend("Clock: OSR_NETTIME=off - using CLOCK_REALTIME.");
  }
  // REALTIME, not MONOTONIC: on a PTP-disciplined host CLOCK_REALTIME already IS the LAN
  // timeline, so PTS stay comparable across machines. This is only true if this host's clock is
  // disciplined - check `timedatectl` / a running ptp4l. If it is not, the stamps are absolute
  // UTC but not aligned; that is still strictly better than a monotonic timeline, which is
  // comparable to nothing, and it is stated plainly rather than assumed.
  GstClock* sys = gst_system_clock_obtain();
  g_object_set(sys, "clock-type", GST_CLOCK_TYPE_REALTIME, NULL);
  logAppend("Clock: using CLOCK_REALTIME - comparable across machines ONLY if this host's clock "
            "is PTP-disciplined (timedatectl / ptp4l).");
  return sys;
}

// Settings every pipeline in a take must agree on, so files from different cameras land on one
// timeline: the shared clock, no start-time offset, the same base-time recipe, and a per-role
// latency.
static void configure_pipeline(GstElement* pipeline, GstClockTime latency)
{
  gst_pipeline_use_clock(GST_PIPELINE(pipeline), osr::app.global_sync_clock);
  gst_element_set_start_time(pipeline, GST_CLOCK_TIME_NONE);
  gst_element_set_base_time(
      pipeline,
      gst_clock_get_time(osr::app.global_sync_clock) + osr::config.base_safety_margin);
  gst_pipeline_set_latency(GST_PIPELINE(pipeline), latency);
}

static void append_note(size_t i, const std::string& text)
{
  {
    std::lock_guard<std::mutex> lock(osr::app.notes_mutex);
    osr::app.source_notes[i].push_back(text);
  }
  logAppend(fmt::format("CAM {}: {}", i, text));
}

static void open_segment_record(size_t i, const std::string& reason)
{
  GDateTime* now = g_date_time_new_now_utc();
  gchar* iso = g_date_time_format_iso8601(now);
  {
    std::lock_guard<std::mutex> lock(osr::app.notes_mutex);
    osr::SegmentRecord rec;
    rec.index = osr::app.source[i].segment.load();
    rec.file = segment_path(i);
    rec.utc_start = iso ? iso : "";
    rec.reason = reason;
    osr::app.source_segments[i].push_back(rec);
  }
  g_free(iso);
  g_date_time_unref(now);
}

static void close_segment_record(size_t i, bool closed)
{
  std::lock_guard<std::mutex> lock(osr::app.notes_mutex);
  if (!osr::app.source_segments[i].empty())
    osr::app.source_segments[i].back().closed = closed;
}

// How long a segment must survive before a failure counts as a fresh problem rather than a
// continuation of the last one. Without this a camera that fails instantly on restart would be
// retried forever at the minimum backoff.
static const GstClockTime kHealthySegmentTime = 10 * GST_SECOND;

// One thread per camera. It owns both of this camera's pipelines for their entire lifetime, so no
// other thread ever touches those pointers.
//
// A fatal error on either pipeline ends only THIS camera: record why, alert the operator, finalise
// what can still be finalised, tear the pair down, and - if the take is still open - rebuild both
// pipelines into a NEW segment file after a backoff. Every other camera keeps recording untouched.
// GStreamer keeps the REASON for a bus error in the message's debug info, not in its text: a
// not-negotiated on a pad arrives as the generic "Internal data stream error." and only the debug
// string says "streaming stopped, reason not-negotiated (-4)" or names the pads and caps involved.
// That is the difference between "the preview did not start" and knowing which link refused, so it
// is logged rather than parsed and dropped.
static std::string gst_error_reason(const gchar* debug_info)
{
  return debug_info != nullptr ? fmt::format(" ({})", debug_info) : std::string{};
}

static void camera_thread(size_t i)
{
  GstElement* capture = nullptr;
  GstElement* encode = nullptr;
  GstBus* capture_bus = nullptr;
  GstBus* encode_bus = nullptr;
  std::atomic_bool encode_eos{false};
  bool running = false;
  int fail_streak = 0;
  GstClockTime segment_started = gst_util_get_timestamp();

  const auto teardown = [&]()
  {
    if (capture_bus) { gst_object_unref(capture_bus); capture_bus = nullptr; }
    if (encode_bus) { gst_object_unref(encode_bus); encode_bus = nullptr; }
    if (capture) { gst_element_set_state(capture, GST_STATE_NULL); gst_object_unref(capture); capture = nullptr; }
    if (encode) { gst_element_set_state(encode, GST_STATE_NULL); gst_object_unref(encode); encode = nullptr; }
    encode_eos.store(false);
    osr::app.source[i].recording.store(false);
    running = false;
  };

  // Producer first: the consumer's intersrc looks the producer up by name when it starts.
  const auto build_and_start = [&]() -> bool
  {
    capture = is_jxs(i) ? build_jxs_capture_pipeline(i) : build_capture_pipeline(i);
    if (!capture)
      return false;
    configure_pipeline(capture, osr::config.capture_latency);
    capture_bus = gst_element_get_bus(capture);
    if (gst_element_set_state(capture, GST_STATE_PLAYING) == GST_STATE_CHANGE_FAILURE) {
      append_note(i, "capture pipeline refused to go to PLAYING");
      return false;
    }

    if (!osr::app.recording.load())
      return true;  // preview-only camera: no consumer side to build

    // A passthrough or ToMkv camera writes its file inside the producer (its file leg
    // IS the recording), so there is no consumer to build for it.
    if (is_jxs(i) && jxs_of(i).mode != osr::JxsRecordMode::Encode) {
      osr::app.source[i].recording.store(true);
      return true;
    }

    encode = build_encode_pipeline(i);
    if (!encode)
      return false;
    configure_pipeline(encode, osr::config.encode_latency);
    encode_bus = gst_element_get_bus(encode);
    if (gst_element_set_state(encode, GST_STATE_PLAYING) == GST_STATE_CHANGE_FAILURE) {
      append_note(i, "encode pipeline refused to go to PLAYING");
      return false;
    }
    osr::app.source[i].recording.store(true);
    return true;
  };

  running = build_and_start();
  if (running && osr::app.recording.load()) {
    open_segment_record(i, "");
    segment_started = gst_util_get_timestamp();
  } else if (!running) {
    append_note(i, "could not start this camera");
  }

  while (osr::app.run.load() && !osr::app.stop_requested.load()) {
    if (!running) {
      // ---- rebuild path -------------------------------------------------
      if (!osr::app.recording.load())
        return;  // preview-only: nothing to rebuild
      if (gst_util_get_timestamp() - segment_started > kHealthySegmentTime)
        fail_streak = 0;  // the last segment ran healthily; treat this as a new problem
      ++fail_streak;
      if (fail_streak > osr::config.restart_max_attempts) {
        osr::app.source[i].gave_up.store(true);
        append_note(i, fmt::format(
            "giving up after {} attempts - this camera is no longer recording. The other "
            "cameras are unaffected.", osr::config.restart_max_attempts));
        return;
      }
      GstClockTime backoff = osr::config.restart_backoff_min;
      for (int k = 1; k < fail_streak; ++k) {
        backoff *= 2;
        if (backoff > osr::config.restart_backoff_max) {
          backoff = osr::config.restart_backoff_max;
          break;
        }
      }
      append_note(i, fmt::format("restart attempt {}/{} in {} ms",
                                 fail_streak, osr::config.restart_max_attempts,
                                 GST_TIME_AS_MSECONDS(backoff)));
      const GstClockTime wake = gst_util_get_timestamp() + backoff;
      while (osr::app.run.load() && !osr::app.stop_requested.load()
             && gst_util_get_timestamp() < wake)
        g_usleep(50 * 1000);
      if (!osr::app.run.load() || osr::app.stop_requested.load())
        return;

      osr::app.source[i].segment.fetch_add(1);
      running = build_and_start();
      if (running) {
        osr::app.source[i].restarts.fetch_add(1);
        open_segment_record(i, "restart");
        segment_started = gst_util_get_timestamp();
        append_note(i, fmt::format("camera restarted into {}", segment_path(i)));
      }
      continue;
    }

    // ---- watch path -----------------------------------------------------
    std::string failure;
    bool saw_eos = false;

    if (capture_bus) {
      GstMessage* msg = gst_bus_timed_pop_filtered(
          capture_bus, 100 * GST_MSECOND,
          static_cast<GstMessageType>(GST_MESSAGE_ERROR | GST_MESSAGE_EOS));
      if (msg) {
        if (GST_MESSAGE_TYPE(msg) == GST_MESSAGE_ERROR) {
          GError* err = nullptr;
          gchar* debug_info = nullptr;
          gst_message_parse_error(msg, &err, &debug_info);
          failure = fmt::format("capture error from {}: {}{}",
                                GST_OBJECT_NAME(msg->src), err ? err->message : "unknown",
                                gst_error_reason(debug_info));
          g_clear_error(&err);
          g_free(debug_info);
        } else {
          saw_eos = true;
        }
        gst_message_unref(msg);
      }
    }
    if (failure.empty() && encode_bus) {
      GstMessage* msg = gst_bus_timed_pop_filtered(
          encode_bus, 100 * GST_MSECOND,
          static_cast<GstMessageType>(GST_MESSAGE_ERROR | GST_MESSAGE_EOS));
      if (msg) {
        if (GST_MESSAGE_TYPE(msg) == GST_MESSAGE_ERROR) {
          GError* err = nullptr;
          gchar* debug_info = nullptr;
          gst_message_parse_error(msg, &err, &debug_info);
          failure = fmt::format("encode error from {}: {}{}",
                                GST_OBJECT_NAME(msg->src), err ? err->message : "unknown",
                                gst_error_reason(debug_info));
          g_clear_error(&err);
          g_free(debug_info);
        } else {
          encode_eos.store(true);
          saw_eos = true;
        }
        gst_message_unref(msg);
      }
    }

    if (failure.empty() && !saw_eos)
      continue;  // quiet tick

    if (!failure.empty()) {
      append_note(i, failure);
      logAppend(fmt::format(
          "CAM {} FAILED: {} -- the other cameras keep recording; this one will be "
          "restarted.", i, failure));
    }

    // Salvage: a healthy consumer needs EOS to write its footer/index. A consumer that already
    // latched an error (mxfmux does, permanently) refuses everything, in which case the segment
    // stays UNFINALISED - recorded as such rather than hidden. It is still readable and
    // repackable: ffmpeg -i <clip> -c copy <fixed>.
    if (encode && !encode_eos.load()) {
      gst_element_send_event(encode, gst_event_new_eos());
      const GstClockTime deadline = gst_util_get_timestamp() + 3 * GST_SECOND;
      while (!encode_eos.load() && gst_util_get_timestamp() < deadline) {
        GstMessage* msg = gst_bus_timed_pop_filtered(encode_bus, 50 * GST_MSECOND, GST_MESSAGE_EOS);
        if (msg) {
          encode_eos.store(true);
          gst_message_unref(msg);
        }
      }
    }
    if (osr::app.recording.load()) {
      const bool closed = (!encode || encode_eos.load());
      close_segment_record(i, closed);
      if (!closed)
        append_note(i, fmt::format(
            "segment {} left UNFINALISED (no index written) - repack it losslessly with: "
            "ffmpeg -i {} -c copy <output>",
            osr::app.source[i].segment.load(), segment_path(i)));
    }
    teardown();
  }

  // Orderly stop: EOS into the producer cascades over the IPC hop into the consumer, which both
  // finalises this camera's file and ends both of its pipelines.
  //
  // detach_requested (the preview -> record transition): the capture pipeline is torn down with
  // NULL instead, so no EOS travels over the hop and the shared preview survives. The inter
  // plugin resumes the preview by itself when the take's new producer registers under the same
  // name.
  if (capture) {
    if (osr::app.detach_requested.load())
      gst_element_set_state(capture, GST_STATE_NULL);
    else
      gst_element_send_event(capture, gst_event_new_eos());   // EOS cascades over the hop
                                                              // and finalises the muxer
  }
  if (encode && !encode_eos.load()) {
    const GstClockTime deadline = gst_util_get_timestamp() + 5 * GST_SECOND;
    while (!encode_eos.load() && gst_util_get_timestamp() < deadline) {
      GstMessage* msg = gst_bus_timed_pop_filtered(encode_bus, 50 * GST_MSECOND, GST_MESSAGE_EOS);
      if (msg) {
        encode_eos.store(true);
        gst_message_unref(msg);
      }
    }
  }
  if (osr::app.recording.load())
    close_segment_record(i, !encode || encode_eos.load());
  teardown();
}

// The preview pipeline is shared by every camera and never restarted: a camera's producers
// re-register under the same names and the preview picks them up by itself.
static void preview_thread_fn()
{
  auto bus = gst_element_get_bus(osr::app.preview_pipeline);
  while (osr::app.run.load() && !osr::app.preview_eos.load()
         && !osr::app.stop_requested.load()) {
    GstMessage* msg = gst_bus_timed_pop_filtered(
        bus, 100 * GST_MSECOND,
        static_cast<GstMessageType>(GST_MESSAGE_ERROR | GST_MESSAGE_EOS));
    if (!msg)
      continue;
    if (GST_MESSAGE_TYPE(msg) == GST_MESSAGE_ERROR) {
      GError* err = nullptr;
      gchar* debug_info = nullptr;
      gst_message_parse_error(msg, &err, &debug_info);
      logAppend(fmt::format("Preview error from {}: {}",
                            GST_OBJECT_NAME(msg->src), err ? err->message : "unknown"));
      // On its own line: the log pane clips a long line on the right, and the reason is the part
      // that matters ("streaming stopped, reason not-negotiated (-4)").
      if (debug_info != nullptr)
        logAppend(fmt::format("Preview error detail: {}", debug_info));
      // An error on the PREVIEW bus is terminal for the preview. The most common
      // one is the operator closing the preview window: xvimagesink posts
      // "Output window was closed" and there is nothing left to show. (Camera
      // failures surface on the per-camera buses, not here.) Tear the preview
      // down so the button resets instead of staying greyed on "Stop Preview".
      osr::app.preview_eos.store(true);
      g_clear_error(&err);
      g_free(debug_info);
    } else {
      osr::app.preview_eos.store(true);
    }
    gst_message_unref(msg);
  }
  gst_element_set_state(osr::app.preview_pipeline, GST_STATE_NULL);
  gst_object_unref(bus);
  osr::app.preview_running.store(false);
  // The preview ended (EOS, an error, or the operator's Stop). Reset the button on the
  // UI thread - a preview that ended by any path other than Stop left it greyed out
  // saying "Stop Preview" forever (the window-close case). The record button only if
  // no take is in flight.
  Fl::lock();
  osr::app.ui->btn_preview->label("Start Preview");
  osr::app.ui->btn_preview->activate();
  if (!osr::app.recording.load()) {
    osr::app.ui->btn_record->label("Start Recording");
    osr::app.ui->btn_record->activate();
  }
  Fl::unlock();
  Fl::awake();
}

// Start (or refresh) the shared preview over the cameras' producers.
static bool start_preview()
{
  if (!osr::app.global_sync_clock)
    osr::app.global_sync_clock = acquire_sync_clock();

  // The tiles are built from the camera set at the moment the pipeline is created, and that pipeline
  // is otherwise never rebuilt - so a camera added after the first Preview (a JPEG XS node beside an
  // HDMI unit, say) had no tile at all and could never appear, however healthy its stream was.
  // Rebuild when the set has moved, tearing the old pipeline down first and only while it is idle.
  static int built_from_generation = -1;
  if (osr::app.preview_pipeline != nullptr && built_from_generation != g_camera_set_generation)
  {
    if (osr::app.preview_running.load())
      logAppend("Preview: the camera set changed - Stop Preview, then Start again, to include it.");
    else
    {
      gst_element_set_state(osr::app.preview_pipeline, GST_STATE_NULL);
      gst_object_unref(osr::app.preview_pipeline);
      osr::app.preview_pipeline = nullptr;
      logAppend("Preview: tiles rebuilt for the new camera set.");
    }
  }
  if (!osr::app.preview_pipeline) {
    build_preview_pipeline();
    if (!osr::app.preview_pipeline) {
      logAppend("Error: preview pipeline could not be created.");
      return false;
    }
    built_from_generation = g_camera_set_generation;
  }
  if (osr::app.preview_running.load())
    return true;
  osr::app.preview_eos.store(false);
  configure_pipeline(osr::app.preview_pipeline, osr::config.preview_latency);
  osr::app.preview_running.store(true);
  // Arm the health indicator loop (a 2 Hz UI-thread timer that rewrites each tile's textoverlay
  // from the real frame counters). It disarms itself when the preview stops.
  if (!g_health_timer_started) {
    g_health_timer_started = true;
    Fl::add_timeout(0.5, health_tick_cb);
  }
  osr::app.preview_thread = std::async(std::launch::async, preview_thread_fn);
  if (gst_element_set_state(osr::app.preview_pipeline, GST_STATE_PLAYING)
      == GST_STATE_CHANGE_FAILURE) {
    logAppend("Error: preview pipeline refused to go to PLAYING.");
    return false;
  }
  return true;
}

// Start a take: one camera thread per source, each owning its own pair of pipelines from here on.
static bool start_session(bool record)
{
  if (camera_count() == 0) {
    logAppend("Error: No sources added.");
    return false;
  }
  if (!osr::app.global_sync_clock)
    osr::app.global_sync_clock = acquire_sync_clock();

  // Reap the previous take's threads; every one of them has returned once a stop has completed.
  osr::app.camera_threads.clear();

  osr::app.stop_requested.store(false);
  osr::app.detach_requested.store(false);
  osr::app.run.store(true);
  osr::app.recording.store(record);
  osr::app.session_active.store(true);

  if (record)
    begin_recording_session();

  size_t n = camera_count();
  if (n > osr::App::kMaxSources)
    n = osr::App::kMaxSources;
  for (size_t i = 0; i < n; ++i) {
    osr::app.source[i].segment.store(1);
    osr::app.source[i].restarts.store(0);
    osr::app.source[i].gave_up.store(false);
    osr::app.source[i].recording.store(false);
    {
      std::lock_guard<std::mutex> lock(osr::app.notes_mutex);
      osr::app.source_segments[i].clear();
      osr::app.source_notes[i].clear();
    }
    osr::app.camera_threads.push_back(std::async(std::launch::async, camera_thread, i));
  }
  return true;
}

// Stop the take: ask the camera threads to finish, then wait for them. Each camera finalises its
// own file - EOS into its own producer side cascades over the IPC hop and makes its muxer write
// the footer/index - and tears down its own pipelines. Nothing is shared, and nothing is left
// running: the old single-pipeline stop was what used to strand a live pipeline and its thread.
//
// detach (the preview -> record transition, called with the UI thread waiting): stop the camera
// threads but leave the shared preview alone. The producers are torn down without EOS so the
// preview never finalises; the take's own producers then re-register under the same names.
static void stop_session_impl(bool detach)
{
  osr::app.stop_requested.store(true);
  osr::app.detach_requested.store(detach);

  if (!detach && osr::app.preview_pipeline && osr::app.preview_running.load())
    gst_element_send_event(osr::app.preview_pipeline, gst_event_new_eos());

  for (auto& thread : osr::app.camera_threads) {
    if (thread.valid())
      thread.wait();
  }
  if (!detach && osr::app.preview_thread.valid())
    osr::app.preview_thread.wait();

  if (osr::app.recording.exchange(false)) {
    osr::app.watchdog_should_run.store(false);
    if (osr::app.watchdog_thread.valid())
      osr::app.watchdog_thread.wait();
    write_recording_sidecars(true);
  }
  osr::app.detach_requested.store(false);
  osr::app.session_active.store(false);
}

// The operator's Stop button (and process exit): full stop, preview included.
static void stop_session()
{
  stop_session_impl(false);
}

void btn_preview_cb(Fl_Button*, void*)
{
  // Wait out a shutdown still in flight so a take never starts on half-torn-down state.
  if (osr::app.shutdown_thread.valid())
    osr::app.shutdown_thread.wait();
  // With a session already running (a take, or preview-only) the camera threads and their
  // producers exist: the preview just attaches to them. Calling start_session() here would
  // clear() a live thread vector, and destroying those futures blocks until their tasks return,
  // which only happens on a stop - that was a UI freeze, not a slow start.
  if (!osr::app.session_active.load()) {
    if (!start_session(false))
      return;
  }
  start_preview();

  Fl::lock();
  osr::app.ui->btn_preview->label("Stop Preview");
  osr::app.ui->btn_preview->deactivate();
  Fl::unlock();
  Fl::awake();
}

void btn_record_cb(Fl_Button*, void*)
{
  if (osr::app.shutdown_thread.valid())
    osr::app.shutdown_thread.wait();

  // A preview-only session is running: end its camera threads, but detach - leave the shared
  // preview up. The take's producers then register under the same names and the preview resumes
  // on them by itself, so the grid keeps showing the sources while the take records. For a
  // preview-only session this join is quick (no encode pipelines to drain), so it runs inline.
  if (osr::app.session_active.load() && !osr::app.recording.load())
    stop_session_impl(true);

  if (!start_session(true))
    return;
  start_preview();

  logAppend(fmt::format("\n\nRecording {} source(s) - one capture/encode pipeline pair per "
                        "camera, so one camera failing cannot stop the others:",
                        camera_count()));
  for (size_t i = 0; i < camera_count()
       && i < osr::App::kMaxSources; ++i) {
    logAppend(fmt::format("  CAM {} -> {}  (restarts write _seg2, _seg3, ...)",
                          i, segment_path(i)));
  }

  Fl::lock();
  osr::app.ui->btn_record->label("Stop Recording");
  osr::app.ui->btn_record->deactivate();
  if (osr::app.preview_running.load()) {
    osr::app.ui->btn_preview->label("Stop Preview");
    osr::app.ui->btn_preview->deactivate();
  }
  Fl::unlock();
  Fl::awake();
}
void cb_codec_select(Fl_Menu_*, void* v)
{
  // Convert the callback user_data pointer to an integer-sized type, then to
  // the enum.
  osr::config.codec =
      static_cast<osr::Codec>(reinterpret_cast<std::intptr_t>(v));
}

void cb_quality_select(Fl_Menu_*, void* v)
{
  // Convert the callback user_data pointer to an integer-sized type, then to
  // the enum.
  osr::config.quality =
      static_cast<osr::Quality>(reinterpret_cast<std::intptr_t>(v));
}

void btn_stop_cb(Fl_Button*, void*)
{
  // Stopping is per camera and waits for each one to finalise its file, so it runs on its own
  // thread rather than blocking the UI.
  if (osr::app.shutdown_thread.valid())
    osr::app.shutdown_thread.wait();
  osr::app.shutdown_thread = std::async(std::launch::async, stop_session);

  Fl::lock();
  osr::app.ui->btn_preview->label("Start Preview");
  osr::app.ui->btn_preview->activate();
  osr::app.ui->btn_record->label("Start Recording");
  osr::app.ui->btn_record->activate();
  Fl::unlock();
  Fl::awake();
}

auto main() -> int
{
  /* init GStreamer */
  g_setenv("GST_DEBUG", "0", TRUE);
  g_setenv("GST_DEBUG_DUMP_DOT_DIR", "./", TRUE);
  g_setenv("GST_DEBUG_COLOR_MODE", "off", TRUE);
  // g_setenv("GST_TRACERS", "latency(flags=pipeline+element)", TRUE);
  g_setenv("GST_DEBUG_FILE", "traces.log", TRUE);

  /* Our GStreamer customisations ship next to the application; GStreamer must be
   * pointed at them before it initialises. */
  osr::configure_gst_plugin_path();

  g_usleep(G_USEC_PER_SEC);  // portable Sleep(1000); start-up settle, see git blame

  gst_init(nullptr, nullptr);

  // No camera's range has been measured yet: make that the value every slot reports, rather than
  // the zero that means "measured limited" (see reset_jxs_declared_ranges).
  reset_jxs_declared_ranges();

  osr::verify_mxfmux_plugin();

  //   std::thread findDevicesThread(findDevices);

  Fl::lock();
  initUi();
  findDevices();
  const int result = Fl::run();

  // Stop everything cleanly before the process goes away. Every camera finalises its own file and
  // its thread is joined, instead of being left running into static destruction.
  osr::app.run.store(false);
  osr::app.stop_requested.store(true);
  stop_session();
  if (osr::app.shutdown_thread.valid())
    osr::app.shutdown_thread.wait();
  if (osr::app.preview_thread.valid())
    osr::app.preview_thread.wait();

  return result;
}
