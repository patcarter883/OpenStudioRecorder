/*
 * osrrange - quantisation-range conversion for the OpenStudioRecorder capture path.
 *
 * Why this exists: a capture unit can pass the source's quantisation range through unchanged
 * (the UGREEN/MacroSilicon stick does). When the camera sends full range and the recording chain
 * expects limited (TV) range, that conversion has to be performed explicitly, because on
 * GStreamer 1.28.x:
 *
 *   - videoconvert converts FORMAT, not colourimetry: declaring colorimetry=...:full on the sink
 *     caps and ...:limited on the src caps is a byte-identical pass-through (measured);
 *   - `range` is not a video/x-raw caps field at all (the pipeline is rejected);
 *   - `colorspace` and `avfilter` are not guaranteed to be installed.
 *
 * A per-plane 256-entry lookup table is exact, branch-free and cheap. It is applied in place, so
 * no frame is copied.
 *
 * YUY2 only, on purpose: that is the format this capture path runs in, and refusing everything
 * else means a mis-wired pipeline fails loudly instead of quietly passing unconverted video.
 *
 * The tables are the standard 8-bit conversions:
 *   full -> limited   Y: 0..255 -> 16..235   (gain 219/255),  chroma: 0..255 -> 16..240 (224/255)
 *   limited -> full   the exact inverse, clamped.
 */

#include <gst/base/gstbasetransform.h>
#include <gst/video/video.h>
#include <gst/gst.h>
#include <string.h>   /* strchr - spelling the rewritten colorimetry the way the caller spelled it */

/* GST_PLUGIN_DEFINE() expands to a struct that names PACKAGE; the build has no config.h. */
#ifndef PACKAGE
#define PACKAGE "OpenStudioRecorder"
#endif

GST_DEBUG_CATEGORY_STATIC(gst_osr_range_debug);
#define GST_CAT_DEFAULT gst_osr_range_debug

/* --- mode enum ---------------------------------------------------------------- */

typedef enum {
  OSR_RANGE_MODE_PASSTHROUGH = 0,
  OSR_RANGE_MODE_FULL_TO_LIMITED,
  OSR_RANGE_MODE_LIMITED_TO_FULL,
} OsrRangeMode;

#define OSR_TYPE_RANGE_MODE (osr_range_mode_get_type())

static GType
osr_range_mode_get_type(void)
{
  static gsize id = 0;

  if (g_once_init_enter(&id)) {
    static const GEnumValue values[] = {
      {OSR_RANGE_MODE_PASSTHROUGH, "Pass samples through unchanged", "passthrough"},
      {OSR_RANGE_MODE_FULL_TO_LIMITED, "Full range to limited (TV) range", "full-to-limited"},
      {OSR_RANGE_MODE_LIMITED_TO_FULL, "Limited (TV) range to full range", "limited-to-full"},
      {0, NULL, NULL},
    };
    GType registered = g_enum_register_static("OsrRangeMode", values);
    g_once_init_leave(&id, registered);
  }
  return (GType)id;
}

/* --- element ------------------------------------------------------------------ */

G_DECLARE_FINAL_TYPE(OsrRange, osr_range, OSR, RANGE, GstBaseTransform)

struct _OsrRange {
  GstBaseTransform parent;

  OsrRangeMode mode;

  /* Rebuilt whenever mode changes, so the chain function is a pure table lookup. */
  guint8 lut_y[256];
  guint8 lut_c[256];

  GstVideoInfo info;
  gboolean have_info;
};

G_DEFINE_TYPE(OsrRange, osr_range, GST_TYPE_BASE_TRANSFORM)

enum {
  PROP_0,
  PROP_MODE,
  N_PROPERTIES,
};

static GParamSpec *properties[N_PROPERTIES];

/* The reason the element exists, so wiring it in without a mode still does the intended thing
 * rather than silently passing video through. The app always names the mode explicitly. */
#define OSR_RANGE_MODE_DEFAULT OSR_RANGE_MODE_FULL_TO_LIMITED

static void
osr_range_rebuild_luts(OsrRange *self)
{
  for (guint i = 0; i < 256; i++) {
    switch (self->mode) {
      case OSR_RANGE_MODE_FULL_TO_LIMITED:
        /* +127 before the divide rounds to nearest instead of truncating. */
        self->lut_y[i] = (guint8)((219 * i + 16 * 255 + 127) / 255);
        self->lut_c[i] = (guint8)((224 * i + 16 * 255 + 127) / 255);
        break;
      case OSR_RANGE_MODE_LIMITED_TO_FULL:
        self->lut_y[i] = (i < 16) ? 0 : (guint8)MIN((i - 16) * 255 / 219, 255);
        self->lut_c[i] = (i < 16) ? 0 : (guint8)MIN((i - 16) * 255 / 224, 255);
        break;
      case OSR_RANGE_MODE_PASSTHROUGH:
      default:
        self->lut_y[i] = self->lut_c[i] = (guint8)i;
        break;
    }
  }
}

static void
osr_range_apply_mode(OsrRange *self)
{
  osr_range_rebuild_luts(self);
  gst_base_transform_set_passthrough(GST_BASE_TRANSFORM(self),
                                     self->mode == OSR_RANGE_MODE_PASSTHROUGH);
}

/* --- the caps this element produces -------------------------------------------------------- *
 *
 * This element changes what the samples MEAN, so its src caps are NOT the sink caps: the declared
 * quantisation range differs. GstBaseTransform's default transform_caps is the identity (it hands
 * the input caps to the other pad unchanged), and that default is what made a pipeline which
 * declares the converted range after this element fail to LINK:
 *
 *     intersrc ! video/x-raw,...,colorimetry=1:3:5:1 ! osrrange ! video/x-raw,...,colorimetry=bt709
 *     -> "could not link osrrange0 to capsfilter2"   (erroneous pipeline; the tile never runs)
 *
 * because the src pad's caps query answered with the sink caps, and full-range caps and
 * limited-range caps do not intersect. It was never about in-place operation: always_in_place only
 * decides whether a buffer is copied, never which caps may be advertised.
 *
 * A second, measured property of caps decides HOW the range has to be spelled here: caps fields are
 * compared TEXTUALLY, so colorimetry=bt709 and colorimetry=2:3:5:1 - the same colorimetry - do not
 * intersect. The rewrite therefore offers both spellings of limited-range BT.709 and lets the peer
 * pick; the one that matches the caller's dialect is offered first.
 */

static GstVideoColorRange
osr_range_input_range(OsrRangeMode mode)
{
  return mode == OSR_RANGE_MODE_LIMITED_TO_FULL ? GST_VIDEO_COLOR_RANGE_16_235
                                                : GST_VIDEO_COLOR_RANGE_0_255;
}

static GstVideoColorRange
osr_range_output_range(OsrRangeMode mode)
{
  return mode == OSR_RANGE_MODE_LIMITED_TO_FULL ? GST_VIDEO_COLOR_RANGE_0_255
                                                : GST_VIDEO_COLOR_RANGE_16_235;
}

/* Is @cinfo BT.709 in limited range - the colorimetry GStreamer also names "bt709"? */
static gboolean
osr_range_is_named_bt709(const GstVideoColorimetry *cinfo)
{
  return cinfo->range == GST_VIDEO_COLOR_RANGE_16_235 &&
         cinfo->matrix == GST_VIDEO_COLOR_MATRIX_BT709 &&
         cinfo->transfer == GST_VIDEO_TRANSFER_BT709 &&
         cinfo->primaries == GST_VIDEO_COLOR_PRIMARIES_BT709;
}

static void
osr_range_append_colorimetry(GstCaps *out, const GstStructure *st, GstVideoColorRange range)
{
  const gchar *value = gst_structure_get_string(st, "colorimetry");
  GstVideoColorimetry cinfo;
  gchar *numeric;
  GstStructure *s;
  gboolean prefer_named;

  /* No colorimetry field, or the field is not a plain string (a range or a list): the range is
   * unconstrained here, so there is nothing to rewrite and nothing may be claimed. */
  if (value == nullptr || !gst_video_colorimetry_from_string(&cinfo, value)) {
    gst_caps_append_structure(out, gst_structure_copy(st));
    return;
  }

  prefer_named = strchr(value, ':') == nullptr;   /* the caller spelled it "bt709", not "2:3:5:1" */
  cinfo.range = range;
  numeric = gst_video_colorimetry_to_string(&cinfo);
  if (numeric == nullptr) {
    gst_caps_append_structure(out, gst_structure_copy(st));
    return;
  }

  /* Keep the caller's dialect first: whatever spelling the range came in as, the converted range
   * is offered in the same style, which is the one the neighbouring caps filter is using. */
  if (prefer_named && osr_range_is_named_bt709(&cinfo)) {
    s = gst_structure_copy(st);
    gst_structure_set(s, "colorimetry", G_TYPE_STRING, "bt709", nullptr);
    gst_caps_append_structure(out, s);
    if (g_strcmp0(numeric, "bt709") != 0) {
      s = gst_structure_copy(st);
      gst_structure_set(s, "colorimetry", G_TYPE_STRING, numeric, nullptr);
      gst_caps_append_structure(out, s);
    }
  } else {
    s = gst_structure_copy(st);
    gst_structure_set(s, "colorimetry", G_TYPE_STRING, numeric, nullptr);
    gst_caps_append_structure(out, s);
    if (osr_range_is_named_bt709(&cinfo) && g_strcmp0(numeric, "bt709") != 0) {
      s = gst_structure_copy(st);
      gst_structure_set(s, "colorimetry", G_TYPE_STRING, "bt709", nullptr);
      gst_caps_append_structure(out, s);
    }
  }
  g_free(numeric);
}

static GstCaps *
osr_range_transform_caps(GstBaseTransform *btrans, GstPadDirection direction, GstCaps *caps,
                         GstCaps *filter)
{
  OsrRange *self = OSR_RANGE(btrans);
  GstCaps *out;
  GstCaps *result;
  GstVideoColorRange range;
  guint n;

  if (self->mode == OSR_RANGE_MODE_PASSTHROUGH) {
    /* Nothing is converted, so the caps really do pass through: the base class is right here. */
    return filter != nullptr ? gst_caps_intersect_full(filter, caps, GST_CAPS_INTERSECT_FIRST)
                             : gst_caps_ref(caps);
  }

  /* @direction is the pad @caps live on, so the result describes the OTHER pad: caps arriving on
   * the sink are converted into the OUTPUT range, and caps requested on the src correspond to an
   * INPUT in the input range. */
  range = (direction == GST_PAD_SINK) ? osr_range_output_range(self->mode)
                                      : osr_range_input_range(self->mode);

  out = gst_caps_new_empty();
  n = gst_caps_get_size(caps);
  for (guint i = 0; i < n; i++)
    osr_range_append_colorimetry(out, gst_caps_get_structure(caps, i), range);

  /* The contract of transform_caps is that the result is a subset of @filter when one is given. */
  if (filter == nullptr)
    return out;

  result = gst_caps_intersect_full(out, filter, GST_CAPS_INTERSECT_FIRST);
  gst_caps_unref(out);
  return result;
}

static void
osr_range_set_property(GObject *object, guint prop_id, const GValue *value, GParamSpec *pspec)
{
  OsrRange *self = OSR_RANGE(object);

  switch (prop_id) {
    case PROP_MODE:
      self->mode = (OsrRangeMode)g_value_get_enum(value);
      osr_range_apply_mode(self);
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID(object, prop_id, pspec);
      break;
  }
}

static void
osr_range_get_property(GObject *object, guint prop_id, GValue *value, GParamSpec *pspec)
{
  OsrRange *self = OSR_RANGE(object);

  switch (prop_id) {
    case PROP_MODE:
      g_value_set_enum(value, self->mode);
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID(object, prop_id, pspec);
      break;
  }
}

static gboolean
osr_range_set_caps(GstBaseTransform *btrans, GstCaps *incaps, GstCaps *outcaps)
{
  OsrRange *self = OSR_RANGE(btrans);

  (void)outcaps;  /* the range conversion never changes the geometry or format */

  if (!gst_video_info_from_caps(&self->info, incaps)) {
    GST_ERROR_OBJECT(self, "cannot parse caps %" GST_PTR_FORMAT, incaps);
    self->have_info = FALSE;
    return FALSE;
  }
  self->have_info = TRUE;
  return TRUE;
}

static GstFlowReturn
osr_range_transform_ip(GstBaseTransform *btrans, GstBuffer *buf)
{
  OsrRange *self = OSR_RANGE(btrans);
  GstVideoFrame frame;
  const guint8 *lut_y;
  const guint8 *lut_c;
  gint width, height, stride;
  guint8 *base;

  if (self->mode == OSR_RANGE_MODE_PASSTHROUGH || !self->have_info)
    return GST_FLOW_OK;

  /* In place: the conversion changes sample values only, so no frame is ever copied. The range the
   * samples now have is declared by transform_caps, which advertises the converted range on the src
   * pad - that is what a pipeline that pins the converted caps after this element negotiates
   * against. */
  if (!gst_video_frame_map(&frame, &self->info, buf, GST_MAP_READ | GST_MAP_WRITE)) {
    GST_ERROR_OBJECT(self, "cannot map buffer for the range conversion");
    return GST_FLOW_ERROR;
  }

  base = GST_VIDEO_FRAME_PLANE_DATA(&frame, 0);
  stride = GST_VIDEO_FRAME_PLANE_STRIDE(&frame, 0);
  width = GST_VIDEO_FRAME_WIDTH(&frame);
  height = GST_VIDEO_FRAME_HEIGHT(&frame);
  lut_y = self->lut_y;
  lut_c = self->lut_c;

  /* YUY2: one 4-byte group per pixel pair -> Y0 U Y1 V. */
  for (gint row = 0; row < height; row++) {
    guint8 *p = base + (gsize)row * stride;
    guint8 *end = p + (gsize)(width & ~1) * 2;

    for (; p < end; p += 4) {
      p[0] = lut_y[p[0]];
      p[1] = lut_c[p[1]];
      p[2] = lut_y[p[2]];
      p[3] = lut_c[p[3]];
    }
  }

  gst_video_frame_unmap(&frame);
  return GST_FLOW_OK;
}

/* Pad templates must outlive class_init: gst_element_class_add_static_pad_template() keeps the
 * pointer, so these cannot be compound literals built on the stack inside class_init. */
static GstStaticPadTemplate sink_template = GST_STATIC_PAD_TEMPLATE("sink",
    GST_PAD_SINK, GST_PAD_ALWAYS,
    GST_STATIC_CAPS(GST_VIDEO_CAPS_MAKE("{ YUY2 }")));

static GstStaticPadTemplate src_template = GST_STATIC_PAD_TEMPLATE("src",
    GST_PAD_SRC, GST_PAD_ALWAYS,
    GST_STATIC_CAPS(GST_VIDEO_CAPS_MAKE("{ YUY2 }")));

static void
osr_range_class_init(OsrRangeClass *klass)
{
  GObjectClass *gobject_class = G_OBJECT_CLASS(klass);
  GstElementClass *element_class = GST_ELEMENT_CLASS(klass);
  GstBaseTransformClass *btrans_class = GST_BASE_TRANSFORM_CLASS(klass);

  gobject_class->set_property = osr_range_set_property;
  gobject_class->get_property = osr_range_get_property;

  properties[PROP_MODE] = g_param_spec_enum("mode", "Mode",
      "Quantisation-range conversion to apply (default full-to-limited)",
      OSR_TYPE_RANGE_MODE, OSR_RANGE_MODE_DEFAULT,
      G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);
  g_object_class_install_properties(gobject_class, N_PROPERTIES, properties);

  gst_element_class_set_static_metadata(element_class,
      "OpenStudioRecorder quantisation-range converter", "Filter/Video",
      "Converts quantisation range in place with a per-plane LUT (YUY2)",
      "OpenStudioRecorder");

  gst_element_class_add_static_pad_template(element_class, &sink_template);
  gst_element_class_add_static_pad_template(element_class, &src_template);

  btrans_class->set_caps = osr_range_set_caps;
  btrans_class->transform_ip = osr_range_transform_ip;
  /* Without this the src pad answers with the sink caps (GstBaseTransform's identity default) and a
   * pipeline that declares the converted range downstream cannot even link. */
  btrans_class->transform_caps = osr_range_transform_caps;
}

static void
osr_range_init(OsrRange *self)
{
  self->mode = OSR_RANGE_MODE_DEFAULT;
  self->have_info = FALSE;
  osr_range_apply_mode(self);
}

/* --- plugin ------------------------------------------------------------------- */

static gboolean
plugin_init(GstPlugin *plugin)
{
  GST_DEBUG_CATEGORY_INIT(gst_osr_range_debug, "osrrange", 0,
                          "OpenStudioRecorder quantisation-range converter");
  return gst_element_register(plugin, "osrrange", GST_RANK_NONE, osr_range_get_type());
}

GST_PLUGIN_DEFINE(GST_VERSION_MAJOR, GST_VERSION_MINOR, osrrange,
    "Quantisation-range conversion (full<->limited) for the OpenStudioRecorder capture path",
    plugin_init, "1.0.0", "LGPL", "OpenStudioRecorder", "https://example.com/")
