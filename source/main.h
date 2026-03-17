#pragma once

#include <gst/gst.h>
#include <FL/Fl.H>
#include <thread>
#include <vector>
#include <future>
#include <atomic>
#include "UserInterface.h"

// Forward declaration of the UI class at global scope so it can be
// referenced as a pointer inside the osr namespace even if the full
// definition is not available here.
class UserInterface;

namespace osr
{

enum class Codec
{
  Cineform,  // avenc_cfhd
  DNxHR,  // avenc_dnxhd
  ProRes  // avenc_prores_ks
};

enum class Quality
{
  Low,
  Medium,
  High
};

struct PipelineElements
{
  GstElement* pipeline;
  GstElement* source;
  GstElement* convert;
  GstElement* resample;
  GstElement* sink;
};

struct Config
{
  // std::vector<GstDevice*> sources;
  std::vector<GstDevice*> video_sources;
  std::vector<GstDevice*> audio_sources;
  std::vector<std::string> filenames;  // Maps to sources: filenames[i] is the
                                       // output file for sources[i]
  Codec codec = Codec::Cineform;
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

};

struct App
{
  UserInterface* ui;

  GstDeviceMonitor* audio_monitor;
  GstDeviceMonitor* video_monitor;

  GstDevice* selected_video_device = nullptr;
  GstDevice* selected_audio_device = nullptr;

  std::atomic_bool pipeline_error = false;
  std::atomic_bool run = true;
  std::atomic_bool req_stop = false;
  std::atomic_bool is_stopping = false;
  std::atomic_bool capture_eos = false;
  std::atomic_bool preview_eos = false;
  std::atomic_bool encode_eos = false;
  std::atomic_bool capture_running = false;
  std::atomic_bool preview_running = false;
  std::atomic_bool encode_running = false;

  std::vector<std::future<void>> threads;

  GstElement* capture_pipeline;  // One per source for capture
  GstElement* encode_pipeline;  // One per source for encoding
  GstElement* preview_pipeline = nullptr;

  std::vector<GstElement*> video_tee_elements;

  GstClock *global_sync_clock;
};
}