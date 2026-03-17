#include <algorithm>
#include <chrono>
#include <cstdint>
#include <iostream>
#include <set>
#include <string>

#include "main.h"

#include <FL/Fl.H>
#include <fmt/core.h>

#include "lib.hpp"

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
    case Codec::Cineform:
      return "avenc_cfhd";
    case Codec::DNxHR:
      return "avenc_dnxhd";
    case Codec::ProRes:
      return "avenc_prores_ks";
    default:
      return "avenc_cfhd";
  }
}

static const char* get_muxer_name(Codec codec)
{
  switch (codec) {
    case Codec::DNxHR:
      return "mxfmux";
    case Codec::Cineform:
    return "qtmux";
    case Codec::ProRes:
      return "matroskamux";  // All use MOV container
    default:
      return "qtmux";
  }
}

static const char* get_filename_extension(Codec codec)
{
  switch (codec) {
    case Codec::Cineform:
      return ".mov";
    case Codec::DNxHR:
      return ".mxf";
    case Codec::ProRes:
      return ".mkv";
    default:
      return ".mov";
  }
}

static const char* get_quality_name(Codec codec)
{
  switch (codec) {
    case Codec::Cineform:
      return "quality";
    case Codec::DNxHR:
    case Codec::ProRes:
      return "profile";
    default:
      return "bitrate";
  }
}

static int get_setting_for_quality(Quality quality, Codec codec)
{
  switch (codec) {
    case Codec::Cineform:
      switch (quality) {
        case Quality::Low:
          return 8;  // Cfhd-q high
        case Quality::Medium:
          return 6;  // Cfhd-q film1
        case Quality::High:
          return 3;  // Cfhd-q film2
        default:
          return 10;
      }
    case Codec::DNxHR:
      switch (quality) {
        case Quality::Low:
          return 2;  // Dnxhd-profile dnxhr_sq
        case Quality::Medium:
          return 3;  // Dnxhd-profile dnxhr_hq
        case Quality::High:
          return 4;  // Dnxhd-profile dnxhr_hqx
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
      break;
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
  delete msgPtr;
  return;
}

void logAppend(std::string logMessage)
{
  auto* logMessageCpy = new std::string;
  *logMessageCpy = _strdup(logMessage.c_str());
  Fl::awake(logAppend_cb, logMessageCpy);
  return;
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

static void refresh_video_sources()
{
  auto device_list = gst_device_monitor_get_devices(osr::app.video_monitor);
  Fl::lock();
  g_list_foreach(
      device_list,
      [](gpointer data, gpointer)
      {
        auto device = ((GstDevice*)(data));
        auto deviceProps = gst_device_get_properties(device);
        if (deviceProps) {
          auto propsString = gst_structure_to_string(deviceProps);

          auto device_name = gst_device_get_display_name(device);

          osr::app.ui->video_source_select->add(
              device_name, 0, (Fl_Callback*)cb_video_source_select, device, 0);

          g_free(propsString);
          gst_structure_free(deviceProps);
        }
      },
      NULL);
  Fl::unlock();
  Fl::awake();
  // g_list_free_full(device_list, (GDestroyNotify)gst_object_unref);
}

static void refresh_audio_sources()
{
  auto device_list = gst_device_monitor_get_devices(osr::app.audio_monitor);
  Fl::lock();
  g_list_foreach(
      device_list,
      [](gpointer data, gpointer)
      {
        auto device = ((GstDevice*)(data));
        auto deviceProps = gst_device_get_properties(device);
        if (deviceProps) {
          auto propsString = gst_structure_to_string(deviceProps);

          auto device_name = gst_device_get_display_name(device);

          osr::app.ui->audio_source_select->add(
              device_name, 0, (Fl_Callback*)cb_audio_source_select, device, 0);
          g_free(propsString);
          gst_structure_free(deviceProps);
        }
      },
      NULL);
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
  osr::app.ui->show(NULL, NULL);
}

void refresh_sources_cb(Fl_Button*, void*)
{
  refresh_video_sources();
  refresh_audio_sources();
}

static void update_source_list()
{
  Fl::lock();
  osr::app.ui->browser_source_list->clear();
  for (size_t i = 0; i < osr::config.video_sources.size(); ++i) {
    auto video_element = osr::config.video_sources[i];
    auto video_element_name = gst_device_get_display_name(video_element);
    auto audio_element = osr::config.audio_sources[i];
    auto audio_element_name = gst_device_get_display_name(audio_element);
    auto display_text = fmt::format("{} / {} -> {}",
                                    video_element_name,
                                    audio_element_name,
                                    osr::config.filenames[i]);
    osr::app.ui->browser_source_list->add(display_text.c_str());
    g_free(video_element_name);
    g_free(audio_element_name);
  }
  Fl::unlock();
  Fl::awake();
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
    int idx = osr::app.ui->browser_source_list->value() - 1;
    osr::config.video_sources.erase(osr::config.video_sources.begin()
                                    + static_cast<ptrdiff_t>(idx));
    osr::config.audio_sources.erase(osr::config.audio_sources.begin()
                                    + static_cast<ptrdiff_t>(idx));
    osr::config.filenames.erase(osr::config.filenames.begin()
                                + static_cast<ptrdiff_t>(idx));
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

static std::string make_video_caps_string()
{
  return fmt::format(
      "video/x-raw, format=YUY2, width={}, height={}, framerate={}/1, "
      "pixel-aspect-ratio=1/1, colorimetry=2:4:0:1, chroma-site=dv",
      osr::config.v_width,
      osr::config.v_height,
      osr::config.v_framerate);
}

static std::string make_audio_caps_string()
{
  return fmt::format("audio/x-raw, format=F32LE, channels={}, rate={}",
                     osr::config.a_channels,
                     osr::config.a_sample_rate);
}

static void build_capture_pipeline()
{
  GError* error = NULL;
  std::string pipelineStr = "";
  // pipelineStr += "multiqueue name=mq0 sync-by-running-time=true use-interleave=true ";

  for (size_t i = 0; i < osr::config.video_sources.size(); ++i) {
    auto video_device_props =
        gst_device_get_properties(osr::config.video_sources[i]);
    auto video_props_string = gst_structure_to_string(video_device_props);
    logAppend(video_props_string);
    g_free(video_props_string);
    auto video_device_path =
        gst_structure_get_string(video_device_props, "device.name");
    pipelineStr += fmt::format(
        "mfvideosrc do-timestamp=true device-name={} ! {} ! timecodestamper auto-resync=false  set=always source=running-time ! ",
        escape(video_device_path, {' '}, '\\'),
        make_video_caps_string());
    pipelineStr += fmt::format(
        "queue max-size-time={} ! intersink name=intersink_v{} "
        "producer-name=video{}sink  ",
        osr::config.max_buffer_time,
        i,
        i);
    gst_structure_free(video_device_props);

    auto audio_device_props =
        gst_device_get_properties(osr::config.audio_sources[i]);
    auto audio_props_string = gst_structure_to_string(audio_device_props);
    logAppend(audio_props_string);
    g_free(audio_props_string);
    auto audio_device_path =
        gst_structure_get_string(audio_device_props, "device.strid");
    pipelineStr += fmt::format(
        "wasapi2src do-timestamp=true device={} low-latency=true ! {} ! ",
        escape(audio_device_path, {' '}, '\\'),
        make_audio_caps_string());
    pipelineStr += fmt::format(
        "queue max-size-time={} ! intersink name=intersink_a{} "
        "producer-name=audio{}sink  ",
        osr::config.max_buffer_time,
        i,
        i);
    gst_structure_free(audio_device_props);
  }

  logAppend(pipelineStr);

  auto pipeline = gst_parse_launch(pipelineStr.c_str(), &error);

  // for (size_t i = 0; i < osr::config.video_sources.size(); ++i) {
  //   GstElement* intersink_v = gst_bin_get_by_name(
  //       GST_BIN(pipeline), fmt::format("intersink_v{}", i).c_str());
  //   GstElement* internal_appsink_v = gst_bin_get_by_name(
  //       GST_BIN(intersink_v), fmt::format("intersink_v{}-appsink", i).c_str());
  //   g_object_set(internal_appsink_v, "sync", FALSE, NULL);
  //   g_object_set(internal_appsink_v, "async", TRUE, NULL);
  //   gst_object_unref(intersink_v);
  //   gst_object_unref(internal_appsink_v);

  //   GstElement* intersink_a = gst_bin_get_by_name(
  //       GST_BIN(pipeline), fmt::format("intersink_a{}", i).c_str());
  //   GstElement* internal_appsink_a = gst_bin_get_by_name(
  //       GST_BIN(intersink_a), fmt::format("intersink_a{}-appsink", i).c_str());
  //   g_object_set(internal_appsink_a, "sync", FALSE, NULL);
  //   g_object_set(internal_appsink_a, "async", TRUE, NULL);
  //   gst_object_unref(intersink_a);
  //   gst_object_unref(internal_appsink_a);
  // }

  if (error) {
    log_pipeline_error("creating capture pipeline", error);
    // Handle error
  } else {
    osr::app.capture_pipeline = pipeline;
  }
}

static void build_preview_pipeline()
{
  GError* error = NULL;
  std::string pipelineStr = "";
  pipelineStr += "compositor name=compositor  ! ";
  pipelineStr += make_video_caps_string();
  pipelineStr += " ! videoconvert ! queue max-size-buffers=3 ! autovideosink ";
  pipelineStr +=
      "audiomixer name=mix ! audioconvert ! queue max-size-buffers=3 ! "
      "autoaudiosink  ";

  for (size_t i = 0; i < osr::config.video_sources.size(); ++i) {
    pipelineStr += fmt::format(
        "intersrc name=intersrc_v{} producer-name=video{}sink ! {} ! "
        "queue max-size-time={} ! ",
        i,
        i,
        make_video_caps_string(),
        osr::config.max_buffer_time);
    pipelineStr += "timeoverlay time-mode=time-code ! queue max-size-buffers=3 ! compositor. ";

    pipelineStr += fmt::format(
        "intersrc name=intersrc_a{} producer-name=audio{}sink ! {} ! queue max-size-time={} ! ",
        i,
        i,
        make_audio_caps_string(),
        osr::config.max_buffer_time);
    pipelineStr += "mix. ";
  }

  auto pipeline = gst_parse_launch(pipelineStr.c_str(), &error);
  logAppend(pipelineStr);

  int row = 0, col = 0;
  const int rows =
      (osr::config.video_sources.size() + osr::config.preview_cols - 1)
      / osr::config.preview_cols;
  const int tile_width = osr::config.v_width / osr::config.preview_cols;
  const int tile_height = osr::config.v_height / rows;

  for (size_t i = 0; i < osr::config.video_sources.size(); ++i) {
    GstElement* intersrc_v = gst_bin_get_by_name(
        GST_BIN(pipeline), fmt::format("intersrc_v{}", i).c_str());
    GstElement* internal_appsrc_v = gst_bin_get_by_name(
        GST_BIN(intersrc_v), fmt::format("intersrc_v{}-appsrc", i).c_str());
    g_object_set(internal_appsrc_v, "do-timestamp", FALSE, NULL);
    g_object_set(internal_appsrc_v, "is-live", TRUE, NULL);
    gst_object_unref(intersrc_v);
    gst_object_unref(internal_appsrc_v);

    GstElement* intersrc_a = gst_bin_get_by_name(
        GST_BIN(pipeline), fmt::format("intersrc_a{}", i).c_str());
    GstElement* internal_appsrc_a = gst_bin_get_by_name(
        GST_BIN(intersrc_a), fmt::format("intersrc_a{}-appsrc", i).c_str());
    g_object_set(internal_appsrc_a, "do-timestamp", FALSE, NULL);
    g_object_set(internal_appsrc_a, "is-live", TRUE, NULL);
    gst_object_unref(intersrc_a);
    gst_object_unref(internal_appsrc_a);

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

static void build_encode_pipeline()
{
  GError* error = NULL;
  std::string pipelineStr = "";
  // pipelineStr += "multiqueue name=mq0 sync-by-running-time=true use-interleave=true ";

  for (size_t i = 0; i < osr::config.video_sources.size(); ++i) {
    pipelineStr += fmt::format(
        "intersrc name=intersrc_v{} producer-name=video{}sink ! {} ! ",
        i,
        i,
        make_video_caps_string());
    pipelineStr += fmt::format(
        "queue max-size-buffers=3 ! videoconvert ! {} name=encoder_v{} threads=1 ! queue max-size-time={} ! "
        "{} name=mux{} start-time=2 ! queue max-size-buffers=3 ! filesink sync=false "
        "async=false location={}{} ",
        osr::get_codec_element_name(osr::config.codec),
        i,
        osr::config.max_buffer_time,
        osr::get_muxer_name(osr::config.codec),
        i,
        osr::config.filenames[i],
        osr::get_filename_extension(osr::config.codec));

    pipelineStr += fmt::format(
        "intersrc name=intersrc_a{} producer-name=audio{}sink ! {} ! ",
        i,
        i,
        make_audio_caps_string());

    pipelineStr += fmt::format(
        "queue max-size-time={} ! audioconvert ! audioresample ! "
        "queue max-size-buffers=3 ! mux{}. ",
        osr::config.max_buffer_time,
        i);
  }

  logAppend(pipelineStr);
  auto pipeline = gst_parse_launch(pipelineStr.c_str(), &error);

  for (size_t i = 0; i < osr::config.video_sources.size(); ++i) {
    GstElement* intersrc_v = gst_bin_get_by_name(
        GST_BIN(pipeline), fmt::format("intersrc_v{}", i).c_str());
    GstElement* internal_appsrc_v = gst_bin_get_by_name(
        GST_BIN(intersrc_v), fmt::format("intersrc_v{}-appsrc", i).c_str());
    g_object_set(internal_appsrc_v, "do-timestamp", FALSE, NULL);
    g_object_set(internal_appsrc_v, "is-live", TRUE, NULL);
    gst_object_unref(intersrc_v);
    gst_object_unref(internal_appsrc_v);

    GstElement* intersrc_a = gst_bin_get_by_name(
        GST_BIN(pipeline), fmt::format("intersrc_a{}", i).c_str());
    GstElement* internal_appsrc_a = gst_bin_get_by_name(
        GST_BIN(intersrc_a), fmt::format("intersrc_a{}-appsrc", i).c_str());
    g_object_set(internal_appsrc_a, "do-timestamp", FALSE, NULL);
    g_object_set(internal_appsrc_a, "is-live", TRUE, NULL);
    gst_object_unref(intersrc_a);
    gst_object_unref(internal_appsrc_a);

    GstElement* encoder_v = gst_bin_get_by_name(
        GST_BIN(pipeline), fmt::format("encoder_v{}", i).c_str());
    g_object_set(
        encoder_v,
        osr::get_quality_name(osr::config.codec),
        osr::get_setting_for_quality(osr::config.quality, osr::config.codec),
        NULL);

        GstElement* tsoffset = gst_bin_get_by_name(
        GST_BIN(pipeline), fmt::format("tsoffset{}", i).c_str());
        auto timecodeoffset = -1 * (gst_clock_get_time(
            osr::app.global_sync_clock));
    g_object_set(
        tsoffset,
        "ts-offset",
        timecodeoffset, 
        NULL);
  }

  if (error) {
    g_print("Error creating encode pipeline: %s\n", error->message);
    g_clear_error(&error);
    // Handle error
  } else {
    osr::app.encode_pipeline = pipeline;
  }
}

static void pipeline_thread(GstElement*& pipeline,
                            std::atomic_bool* running_flag,
                          std::atomic_bool* eos_flag)
{
  auto bus = gst_element_get_bus(pipeline);

  do {
    GstMessage* msg = gst_bus_timed_pop_filtered(
        bus,
        1000 * GST_MSECOND,
        static_cast<GstMessageType>(GST_MESSAGE_STATE_CHANGED
                                    | GST_MESSAGE_ERROR | GST_MESSAGE_EOS));
    /* Parse message */
    if (msg != NULL) {
      GError* err;
      gchar* debug_info;

      switch (GST_MESSAGE_TYPE(msg)) {
        case GST_MESSAGE_ERROR:
          gst_message_parse_error(msg, &err, &debug_info);
          logAppend(fmt::format("Error received from element {}: {}\n",
                               GST_OBJECT_NAME(msg->src),
                               err->message));
          logAppend(fmt::format("Debugging information: {}\n",
                               debug_info ? debug_info : "none"));
          g_clear_error(&err);
          g_free(debug_info);
          osr::app.pipeline_error.store(true);
          break;
        case GST_MESSAGE_EOS:
            // GST_DEBUG_BIN_TO_DOT_FILE_WITH_TS(GST_BIN(pipeline),
            //                                   GST_DEBUG_GRAPH_SHOW_ALL,
            //                                   "pipeline_eos.dot");
          logAppend(fmt::format("End-Of-Stream reached.\n"));
          gst_element_set_state(pipeline, GST_STATE_READY);
          break;
        case GST_MESSAGE_STATE_CHANGED:
          /* We are only interested in state-changed messages from the pipeline
           */
          if (GST_MESSAGE_SRC(msg) == GST_OBJECT(pipeline)) {
            GstState old_state, new_state, pending_state;
            gst_message_parse_state_changed(
                msg, &old_state, &new_state, &pending_state);
            logAppend(fmt::format("Pipeline state changed from {} to {}:\n",
                                 gst_element_state_get_name(old_state),
                                 gst_element_state_get_name(new_state)));

            if (old_state == GST_STATE_PAUSED && new_state == GST_STATE_READY) {
              gst_element_set_state(pipeline, GST_STATE_NULL);
              eos_flag->store(true);
            }
            break;
          }
        default:
          /* We should not reach here */

          break;
      }
      gst_message_unref(msg);
    }
  } while (osr::app.run.load() && !eos_flag->load());

  if (osr::app.pipeline_error.load()) {
    /* code */
  }
  gst_element_set_state(pipeline, GST_STATE_NULL);
  gst_object_unref(bus);
  gst_object_unref(pipeline);
  pipeline = nullptr;

  running_flag->store(false);
}

static void run_pipeline(GstElement*& pipeline, std::atomic_bool& running_flag, std::atomic_bool& preroll_flag)
{
  osr::app.is_stopping.store(false);
  osr::app.req_stop.store(false);
  osr::app.run.store(true);

  running_flag.store(true);

  osr::app.threads.push_back(std::async(
      std::launch::async, pipeline_thread, std::ref(pipeline), &running_flag, &preroll_flag));

  auto ret = gst_element_set_state(pipeline, GST_STATE_PLAYING);

  if (ret == GST_STATE_CHANGE_FAILURE) {
    logAppend("Unable to set the pipeline to the playing state.\n");
    osr::app.pipeline_error.store(true);
  }
}

static auto stop()
{
  if (osr::app.encode_running.load()) {
    gst_element_send_event(osr::app.encode_pipeline, gst_event_new_eos());
  } else if (osr::app.preview_running.load()) {
    gst_element_send_event(osr::app.preview_pipeline, gst_event_new_eos());
  } else if (osr::app.capture_running.load()) {
    gst_element_send_event(osr::app.capture_pipeline, gst_event_new_eos());
  }
}

static void start_capture_pipeline()
{
  osr::app.capture_eos = false;
  osr::app.preview_eos = false;
  osr::app.encode_eos = false;

  osr::app.global_sync_clock = gst_system_clock_obtain();
  g_object_set(osr::app.global_sync_clock, "clock-type", GST_CLOCK_TYPE_MONOTONIC, NULL);

  gst_pipeline_use_clock(GST_PIPELINE(osr::app.capture_pipeline),
                         osr::app.global_sync_clock);
  gst_element_set_start_time(osr::app.capture_pipeline, GST_CLOCK_TIME_NONE);
  gst_element_set_base_time(osr::app.capture_pipeline, gst_clock_get_time(
      osr::app.global_sync_clock) + osr::config.base_safety_margin);
  gst_pipeline_set_latency(GST_PIPELINE(osr::app.capture_pipeline), osr::config.capture_latency);
  run_pipeline(osr::app.capture_pipeline, osr::app.capture_running, osr::app.capture_eos);
}

void btn_preview_cb(Fl_Button*, void*)
{
  if (!osr::config.video_sources.empty()) {
    osr::app.is_stopping.store(false);
    osr::app.req_stop.store(false);
    osr::app.run.store(true);

    if (!osr::app.capture_pipeline) {
      build_capture_pipeline();
    }
    if (!osr::app.preview_pipeline) {
      build_preview_pipeline();
    }

    if (!osr::app.capture_running.load() && osr::app.capture_pipeline) {
      start_capture_pipeline();
    }

    if (!osr::app.preview_running.load() && osr::app.preview_pipeline) {
      gst_pipeline_use_clock(
          GST_PIPELINE(osr::app.preview_pipeline),
          osr::app.global_sync_clock);
      gst_element_set_start_time(osr::app.preview_pipeline, GST_CLOCK_TIME_NONE);
      gst_element_set_base_time(
          osr::app.preview_pipeline,
          gst_element_get_base_time(osr::app.capture_pipeline));
      gst_pipeline_set_latency(GST_PIPELINE(osr::app.preview_pipeline), osr::config.preview_latency);
      run_pipeline(osr::app.preview_pipeline, osr::app.preview_running, osr::app.preview_eos);


                logAppend("\n\nPipeline Times:");

      logAppend("Pipe\t\tBase\t\tLatency");

      logAppend(fmt::format("Capture\t\t{}\t\t{}",
                                  GST_TIME_AS_SECONDS(
                               gst_element_get_base_time(
                                   osr::app.capture_pipeline)),
                                  GST_TIME_AS_MSECONDS(gst_pipeline_get_latency(
                                      GST_PIPELINE(osr::app.capture_pipeline)))));

      logAppend(fmt::format("Preview\t\t{}\t\t{}",
                                  GST_TIME_AS_SECONDS(
                               gst_element_get_base_time(
                                   osr::app.preview_pipeline)),
                                  GST_TIME_AS_MSECONDS(gst_pipeline_get_latency(
                                      GST_PIPELINE(osr::app.preview_pipeline)))));
    }

    Fl::lock();
    osr::app.ui->btn_preview->label("Stop Preview");
    osr::app.ui->btn_preview->deactivate();
    Fl::unlock();
    Fl::awake();

  } else {
    logAppend("Error: No sources added.");
  }
}

void btn_record_cb(Fl_Button*, void*)
{
  if (!osr::config.video_sources.empty()) {
    osr::app.is_stopping.store(false);
    osr::app.req_stop.store(false);
    osr::app.run.store(true);
    if (!osr::app.capture_pipeline) {
      build_capture_pipeline();
    }
    if (!osr::app.encode_pipeline) {
      build_encode_pipeline();
    }

    if (!osr::app.capture_running.load() && osr::app.capture_pipeline) {
      start_capture_pipeline();
    }

    if (!osr::app.encode_running.load() && osr::app.encode_pipeline) {
      gst_pipeline_use_clock(
          GST_PIPELINE(osr::app.encode_pipeline),
          osr::app.global_sync_clock);
      gst_element_set_start_time(osr::app.encode_pipeline, GST_CLOCK_TIME_NONE);
      gst_element_set_base_time(
          osr::app.encode_pipeline,
          gst_clock_get_time(osr::app.global_sync_clock)
          + osr::config.base_safety_margin);
      gst_pipeline_set_latency(GST_PIPELINE(osr::app.encode_pipeline), osr::config.encode_latency);
      run_pipeline(osr::app.encode_pipeline, osr::app.encode_running, osr::app.encode_eos);

      logAppend("\n\nPipeline Times:");

      logAppend("Pipe\t\tBase\t\tLatency");

      logAppend(fmt::format("Capture\t\t{}\t\t{}",
                                  GST_TIME_AS_SECONDS(
                               gst_element_get_base_time(
                                   osr::app.capture_pipeline)),
                                  GST_TIME_AS_MSECONDS(gst_pipeline_get_latency(
                                      GST_PIPELINE(osr::app.capture_pipeline)))));

      logAppend(fmt::format("Preview\t\t{}\t\t{}",
                                  GST_TIME_AS_SECONDS(
                               gst_element_get_base_time(
                                   osr::app.preview_pipeline)),
                                  GST_TIME_AS_MSECONDS(gst_pipeline_get_latency(
                                      GST_PIPELINE(osr::app.preview_pipeline)))));

      logAppend(fmt::format("Encode\t\t{}\t\t{}",
                                  GST_TIME_AS_SECONDS(
                               gst_element_get_base_time(
                                   osr::app.encode_pipeline)),
                                  GST_TIME_AS_MSECONDS(gst_pipeline_get_latency(
                                      GST_PIPELINE(osr::app.encode_pipeline)))));
    }

    Fl::lock();
    osr::app.ui->btn_record->label("Stop Recording");
    osr::app.ui->btn_record->deactivate();
    Fl::unlock();
    Fl::awake();

  } else {
    logAppend("Error: No sources added.");
  }
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
  stop();
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

  Sleep(1000);

  gst_init(nullptr, nullptr);

  //   std::thread findDevicesThread(findDevices);

  Fl::lock();
  initUi();
  findDevices();
  const int result = Fl::run();

  //   if (findDevicesThread.joinable()) {
  //     findDevicesThread.join();
  //   }

  return result;
}
