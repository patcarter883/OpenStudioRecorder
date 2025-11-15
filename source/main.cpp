#include <algorithm>
#include <chrono>
#include <future>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

#include <FL/Fl.H>
#include <fmt/core.h>
#include <gst/gst.h>

#include <UserInterface.cxx>

#include "lib.hpp"

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
  std::vector<GstElement*> sources;
  std::vector<std::string> filenames;  // Maps to sources: filenames[i] is the
                                       // output file for sources[i]
  Codec codec = Codec::Cineform;
  Quality quality = Quality::High;
};

struct App
{
  UserInterface* ui = new UserInterface;
  /*Fl_Text_Buffer* log_buff = new Fl_Text_Buffer;
  Fl_Text_Buffer* rist_log_buff = new Fl_Text_Buffer;*/

  GstDeviceMonitor* monitor;

  std::atomic_bool pipeline_error = false;
  std::atomic_bool is_previewing = false;
  std::atomic_bool run = true;
  std::atomic_bool is_stopping = false;

  std::vector<std::future<void>> threads;

  GstElement* capture_pipeline;  // One per source for capture
  GstElement* encode_pipeline;  // One per source for encoding
  GstElement* preview_pipeline = nullptr;
};

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
    case Codec::ProRes:
      return "qtmux";  // All use MOV container
    default:
      return "qtmux";
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
          return 12;  // Cfhd-q low
        case Quality::Medium:
          return 10;  // Cfhd-q medium
        case Quality::High:
          return 8;  // Cfhd-q high
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

static void refresh_sources(){
		auto device_list = gst_device_monitor_get_devices(osr::app.monitor);
	Fl::lock();
	  g_list_foreach(
      device_list,
      [](gpointer data, gpointer user_data)
      {
        auto device = ((GstDevice*)(data));
        auto deviceProps = gst_device_get_properties(device);
        auto propsString = gst_structure_to_string(deviceProps);
        if (propsString) {
          osr::log(propsString);
        }
        g_free(propsString);
        gst_structure_free(deviceProps);

        auto device_name = gst_device_get_display_name(device);
         
      osr::app.ui->source_select->add(
          device_name, 0, 0, device_name, 0);
      
      },
      NULL);
	  Fl::unlock();
      Fl::awake();
	  g_list_free_full(device_list, (GDestroyNotify)gst_object_unref);
}

static void* findDevices()
{
  GstBus* bus;
  GstCaps* caps;

  osr::app.monitor = gst_device_monitor_new();
  bus = gst_device_monitor_get_bus(osr::app.monitor);
  caps = gst_caps_new_any();
  gst_device_monitor_add_filter(osr::app.monitor, "Video/Source", caps);
  gst_caps_unref(caps);

  refresh_sources();

  return 0L;
}

static void initUi()
{
  osr::app.ui->show(NULL, NULL);
}

void refresh_sources_cb(Fl_Button*, void*) {
	refresh_sources();
}

static void update_source_list()
{
  Fl::lock();
  osr::app.ui->browser_source_list->clear();
  for (size_t i = 0; i < osr::config.sources.size(); ++i) {
    auto element = osr::config.sources[i];
    auto element_name = gst_element_get_name(element);
    auto display_text =
        fmt::format("{} -> {}", element_name, osr::config.filenames[i]);
    osr::app.ui->browser_source_list->add(display_text.c_str());
    g_free(element_name);
  }
  Fl::unlock();
  Fl::awake();
}

void add_source_cb(Fl_Button*, void*)
{
  const char* filename = osr::app.ui->filename_input->value();
  if (!filename || strlen(filename) == 0) {
    osr::log("Error: Please enter a target filename before adding a source.");
    return;
  }

  auto device_list = gst_device_monitor_get_devices(osr::app.monitor);

  g_list_foreach(
      device_list,
      [](gpointer data, gpointer user_data)
      {
        auto device = ((GstDevice*)(data));
        auto deviceProps = gst_device_get_properties(device);
        auto propsString = gst_structure_to_string(deviceProps);
        if (propsString) {
          osr::log(propsString);
        }
        g_free(propsString);
        gst_structure_free(deviceProps);

        auto device_name = gst_device_get_display_name(device);
        auto source_name = osr::app.ui->source_select->mvalue()->text;
        if (strcmp(device_name, source_name) == 0) {
          osr::config.sources.push_back(
              gst_device_create_element(device, device_name));
          osr::config.filenames.push_back(std::string((const char*)user_data));
        }
      },
      (gpointer)filename);

  osr::app.ui->filename_input->value("");
  update_source_list();
}

void remove_source_cb(Fl_Button*, void*)
{
  if (osr::app.ui->browser_source_list->value()) {
    int idx = osr::app.ui->browser_source_list->value() - 1;
    osr::config.sources.erase(osr::config.sources.begin()
                              + static_cast<ptrdiff_t>(idx));
    osr::config.filenames.erase(osr::config.filenames.begin()
                                + static_cast<ptrdiff_t>(idx));
    update_source_list();
  }
}

static void build_capture_pipeline()
{
  // Create capture pipeline: source -> timestamping -> convert -> IPC output
  // (shmsink)

  auto pipeline = gst_pipeline_new(0);

  for (size_t i = 0; i < osr::config.sources.size(); ++i) {
    auto source = osr::config.sources[i];
    auto source_idx = i;
    auto convert = gst_element_factory_make("videoconvert", 0);
    auto shmsink = gst_element_factory_make("intervideosink", 0);
    auto queue = gst_element_factory_make("queue", 0);

    if (!convert || !shmsink || !queue) {
      osr::log("Error: Failed to create capture pipeline elements.");
      osr::app.pipeline_error.store(true);
      return;
    }

    gst_bin_add_many(GST_BIN(pipeline), source, queue, convert, shmsink, NULL);

    if (!gst_element_link_many(source, queue, convert, shmsink, NULL)) {
      osr::log("Error: Failed to link capture pipeline elements.");
      gst_object_unref(pipeline);
      osr::app.pipeline_error.store(true);
      return;
    }

    g_object_set(shmsink,
                 "channel",
                 osr::get_ipc_pipeline_name(source_idx).c_str(),
                 NULL);
    osr::log(fmt::format("Capture pipeline created for source {}", source_idx));
  }

  osr::app.capture_pipeline = pipeline;
}

static void build_encode_pipeline()
{
  // Create encode pipeline: shmsrc -> encoder -> muxer -> filesink
  // No format conversion or display; assumes IPC provides pre-converted video

  auto pipeline = gst_pipeline_new(0);

  for (size_t i = 0; i < osr::config.sources.size(); ++i) {
    size_t source_idx = i;
    std::string filename = osr::config.filenames[i];

    auto shmsrc = gst_element_factory_make("intervideosrc", 0);
    auto queue = gst_element_factory_make("queue", 0);
    auto convert = gst_element_factory_make("videoconvert", 0);
    auto encoder = gst_element_factory_make(
        osr::get_codec_element_name(osr::config.codec), 0);
    auto mux =
        gst_element_factory_make(osr::get_muxer_name(osr::config.codec), 0);
    auto filesink = gst_element_factory_make("filesink", 0);

    if (!shmsrc || !queue || !convert || !encoder || !mux || !filesink) {
      osr::log("Error: Failed to create encode pipeline elements.");
      osr::app.pipeline_error.store(true);
      return;
    }

    g_object_set(shmsrc,
                 "channel",
                 osr::get_ipc_pipeline_name(source_idx).c_str(),
                 NULL);

    // Set encoder bitrate based on quality
    int bitrate =
        osr::get_setting_for_quality(osr::config.quality, osr::config.codec);
    g_object_set(
        encoder, osr::get_quality_name(osr::config.codec), bitrate, NULL);

    // Set output filename
    g_object_set(filesink, "location", filename.c_str(), NULL);

    gst_bin_add_many(GST_BIN(pipeline),
                     shmsrc,
                     queue,
                     convert,
                     encoder,
                     mux,
                     filesink,
                     NULL);

    if (!gst_element_link_many(
            shmsrc, queue, convert, encoder, mux, filesink, NULL))
    {
      osr::log("Error: Failed to link encode pipeline elements.");
      gst_object_unref(pipeline);
      osr::app.pipeline_error.store(true);
      return;
    }

    osr::log(fmt::format(
        "Encode pipeline created for source {} -> {}", source_idx, filename));
  }
  osr::app.encode_pipeline = pipeline;
}

static void build_preview_pipeline()
{
  if (osr::config.sources.empty()) {
    osr::log("No sources available for preview.");
    return;
  }

  // Clear previous state
  osr::app.capture_pipeline = nullptr;
  osr::app.encode_pipeline = nullptr;
  osr::app.preview_pipeline = nullptr;

  build_capture_pipeline();

  if (osr::app.pipeline_error) {
    return;
  }

  // Create main preview display pipeline
  auto preview_pipeline = gst_pipeline_new("preview_pipeline");
  auto compositor = gst_element_factory_make("compositor", "compositor");
  auto videoconvert =
      gst_element_factory_make("videoconvert", "preview_convert");
  auto videosink = gst_element_factory_make("autovideosink", "preview_sink");

  if (!compositor || !videoconvert || !videosink) {
    osr::log("Error: Failed to create preview pipeline elements.");
    gst_object_unref(preview_pipeline);
    osr::app.pipeline_error.store(true);
    return;
  }

  // Set compositor properties for tiling layout
  g_object_set(compositor, "background", 1, NULL);  // Black background

  // Add elements to pipeline
  gst_bin_add_many(
      GST_BIN(preview_pipeline), compositor, videoconvert, videosink, NULL);

  // Link compositor -> videoconvert -> videosink
  if (!gst_element_link_many(compositor, videoconvert, videosink, NULL)) {
    osr::log("Error: Failed to link preview pipeline elements.");
    gst_object_unref(preview_pipeline);
    osr::app.pipeline_error.store(true);
    return;
  }

  // Create shmsrc elements for each source and connect to compositor pads
  int row = 0, col = 0;
  const int cols = 2;  // 2 columns for tiling
  const int tile_width = 960;
  const int tile_height = 540;

  for (size_t i = 0; i < osr::config.sources.size(); ++i) {
    // Create shmsrc element to receive video from capture pipeline via IPC
    auto shmsrc = gst_element_factory_make("intervideosrc", nullptr);
    auto queue = gst_element_factory_make("queue", nullptr);
    auto videoconvert_src = gst_element_factory_make("videoconvert", nullptr);

    if (!shmsrc || !queue || !videoconvert_src) {
      osr::log("Error: Failed to create preview source elements.");
      osr::app.pipeline_error.store(true);
      return;
    }

    // Set shmsrc socket name to match capture pipeline output
    g_object_set(
        shmsrc, "channel", osr::get_ipc_pipeline_name(i).c_str(), NULL);

    // Add elements to preview pipeline
    gst_bin_add_many(
        GST_BIN(preview_pipeline), shmsrc, queue, videoconvert_src, NULL);

    // Link shmsrc -> queue -> videoconvert -> videoscale -> capsfilter
    if (!gst_element_link_many(shmsrc, queue, videoconvert_src, NULL)) {
      osr::log("Error: Failed to link preview source chain.");
      osr::app.pipeline_error.store(true);
      return;
    }

    // Get pad from capsfilter and link to compositor
    auto src_pad = gst_element_get_static_pad(videoconvert_src, "src");
    auto compositor_sink_pad =
        gst_element_request_pad_simple(compositor, "sink_%u");

    if (!src_pad || !compositor_sink_pad) {
      osr::log("Error: Failed to get pads for compositor linking.");
      if (src_pad)
        gst_object_unref(src_pad);
      if (compositor_sink_pad)
        gst_object_unref(compositor_sink_pad);
      osr::app.pipeline_error.store(true);
      return;
    }

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
                 NULL);

    // Link and release pads
    if (gst_pad_link(src_pad, compositor_sink_pad) != GST_PAD_LINK_OK) {
      osr::log("Error: Failed to link to compositor.");
      gst_object_unref(src_pad);
      gst_object_unref(compositor_sink_pad);
      osr::app.pipeline_error.store(true);
      return;
    }

    gst_object_unref(src_pad);
    gst_object_unref(compositor_sink_pad);

    // Update grid position
    col++;
    if (col >= cols) {
      col = 0;
      row++;
    }
  }

  osr::app.preview_pipeline = preview_pipeline;
  osr::log("Preview pipeline created successfully.");
}

static void pipeline_thread(GstElement* pipeline)
{
  auto bus = gst_element_get_bus(pipeline);

  do {
    GstMessage* msg = gst_bus_timed_pop_filtered(
        bus,
        100,
        static_cast<GstMessageType>(GST_MESSAGE_STATE_CHANGED
                                    | GST_MESSAGE_ERROR | GST_MESSAGE_EOS));
    /* Parse message */
    if (msg != NULL) {
      GError* err;
      gchar* debug_info;

      switch (GST_MESSAGE_TYPE(msg)) {
        case GST_MESSAGE_ERROR:
          gst_message_parse_error(msg, &err, &debug_info);
          osr::log(fmt::format("Error received from element {}: {}\n",
                               GST_OBJECT_NAME(msg->src),
                               err->message));
          osr::log(fmt::format("Debugging information: {}\n",
                               debug_info ? debug_info : "none"));
          g_clear_error(&err);
          g_free(debug_info);
          osr::app.pipeline_error.store(true);
          break;
        case GST_MESSAGE_EOS:
          osr::log(fmt::format("End-Of-Stream reached.\n"));
          osr::app.is_stopping.store(true);
          break;
        case GST_MESSAGE_STATE_CHANGED:
          /* We are only interested in state-changed messages from the pipeline
           */
          if (GST_MESSAGE_SRC(msg) == GST_OBJECT(pipeline)) {
            GstState old_state, new_state, pending_state;
            gst_message_parse_state_changed(
                msg, &old_state, &new_state, &pending_state);
            osr::log(fmt::format("Pipeline state changed from {} to {}:\n",
                                 gst_element_state_get_name(old_state),
                                 gst_element_state_get_name(new_state)));
			if (osr::app.is_stopping.load()) {
				switch (new_state)
				{
				case GST_STATE_PLAYING:
					gst_element_set_state(pipeline, GST_STATE_PAUSED);
					break;
				case GST_STATE_PAUSED:
					gst_element_set_state(pipeline, GST_STATE_READY);
					break;

				case GST_STATE_READY:
					gst_element_set_state(pipeline, GST_STATE_NULL);
					break;

				case GST_STATE_NULL:
					osr::app.run.store(false);
					break;
				
				default:
					break;
				}
			}
          }
          break;
        default:
          /* We should not reach here */
          osr::log("Unexpected message received.\n");
          break;
      }
      gst_message_unref(msg);
    }
	if (osr::app.is_stopping.load()) {
		gst_element_set_state(pipeline, GST_STATE_PAUSED);
	}

  } while (!osr::app.run.load());

  if (osr::app.pipeline_error.load()) {
    /* code */
  }   

  gst_object_unref(bus);
  gst_object_unref(pipeline);
}

static void run_pipeline(GstElement* pipeline)
{
  osr::app.threads.push_back(
      std::async(std::launch::async, pipeline_thread, pipeline));

  auto ret = gst_element_set_state(pipeline, GST_STATE_PLAYING);

  if (ret == GST_STATE_CHANGE_FAILURE) {
    osr::log("Unable to set the pipeline to the playing state.\n");
    osr::app.pipeline_error.store(true);
  }
}

static auto run_preview()
{
  osr::app.run.store(false);

  // Build and start capture pipelines
  build_preview_pipeline();
  if (osr::app.pipeline_error) {
    return false;
  }

  // Start all capture pipelines
  osr::app.threads.push_back(std::async(
      std::launch::async, pipeline_thread, osr::app.capture_pipeline));

  auto ret =
      gst_element_set_state(osr::app.capture_pipeline, GST_STATE_PLAYING);
  if (ret == GST_STATE_CHANGE_FAILURE) {
    osr::log("Unable to set capture pipeline to playing state.");
    osr::app.pipeline_error.store(true);
    return false;
  }

  // Run preview pipeline in a thread
  osr::app.threads.push_back(std::async(
      std::launch::async, pipeline_thread, osr::app.preview_pipeline));
  // Start preview pipeline
  auto preview_ret =
      gst_element_set_state(osr::app.preview_pipeline, GST_STATE_PLAYING);
  if (preview_ret == GST_STATE_CHANGE_FAILURE) {
    osr::log("Unable to set preview pipeline to playing state.");
    osr::app.pipeline_error.store(true);
    return false;
  }

  return true;
}

static auto run_recording()
{

  // Build and start capture pipelines if not already running
  if (!osr::app.is_previewing) {
    run_preview();
    if (osr::app.pipeline_error) {
      return false;
    }
  }

  // Build and start encoding pipelines
  build_encode_pipeline();

  if (!osr::app.pipeline_error) {
    osr::app.threads.push_back(std::async(
        std::launch::async, pipeline_thread, osr::app.encode_pipeline));
    auto ret =
        gst_element_set_state(osr::app.encode_pipeline, GST_STATE_PLAYING);
    if (ret == GST_STATE_CHANGE_FAILURE) {
      osr::log("Unable to set encode pipeline to playing state.");
      osr::app.pipeline_error.store(true);
      return false;
    }

    return true;
  }
  return false;
}

static auto stop()
{
  osr::app.is_stopping.store(true);
}

void btn_preview_cb(Fl_Button*, void*)
{
  if (!osr::config.sources.empty()) {
    if (run_preview()) {
      Fl::lock();
      osr::app.ui->btn_preview->label("Stop Preview");
      osr::app.ui->btn_preview->deactivate();
      Fl::unlock();
      Fl::awake();
    }
  } else {
    osr::log("Error: No sources added.");
  }
}

void btn_record_cb(Fl_Button*, void*)
{
  if (!osr::config.sources.empty()) {
    if (run_recording()) {
      Fl::lock();
      osr::app.ui->btn_record->label("Stop Recording");
      osr::app.ui->btn_record->deactivate();
      Fl::unlock();
      Fl::awake();
    }
  } else {
    osr::log("Error: No sources added.");
  }
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

  gst_init(nullptr, nullptr);

  if (!gst_debug_is_active()) {
    gst_debug_set_active(TRUE);
    gst_debug_set_default_threshold(GST_LEVEL_DEBUG);
  }

  

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
