/* GStreamer
 * Copyright (C) 2025 FIXME <fixme@example.com>
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Library General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Library General Public License for more details.
 *
 * You should have received a copy of the GNU Library General Public
 * License along with this library; if not, write to the
 * Free Software Foundation, Inc., 51 Franklin Street, Suite 500,
 * Boston, MA 02110-1335, USA.
 */
/**
 * SECTION:element-gstrgavideoconvert
 *
 * The rgavideoconvert element does FIXME stuff.
 *
 * <refsect2>
 * <title>Example launch line</title>
 * |[
 * gst-launch-1.0 -v fakesrc ! video/x-raw,format=NV12,width=1920,height=1080 !
 * rgavideoconvert ! video/x-raw,format=RGBA,width=640,height=480 ! fakesink
 * ]|
 * convert 1920x1080 ---> 640x480 and NV12 ---> RGBA .
 * </refsect2>
 */

#include "gst/gstpluginfeature.h"
#ifdef HAVE_CONFIG_H
#include "config.h"  // NOLINT
#endif

#include <gst/allocators/gstdmabuf.h>
#include <gst/gst.h>
#include <gst/video/gstvideofilter.h>
#include <gst/video/video.h>

#include "gstrgavideoconvert.h"  // NOLINT
#include "rga/RgaApi.h"
#include "rga/im2d.h"

GST_DEBUG_CATEGORY_STATIC(gst_rga_video_convert_debug_category);
#define GST_CAT_DEFAULT gst_rga_video_convert_debug_category

#define GST_CASE_RETURN(a, b) \
  case a:                     \
    return b

/* prototypes */

static gboolean gst_rga_video_convert_start(GstBaseTransform *trans);
static gboolean gst_rga_video_convert_stop(GstBaseTransform *trans);

static GstCaps *gst_rga_video_convert_transform_caps(GstBaseTransform *trans,
                                                     GstPadDirection direction,
                                                     GstCaps *caps,
                                                     GstCaps *filter);

static GstCaps *gst_rga_video_convert_fixate_caps(GstBaseTransform *trans,
                                                   GstPadDirection direction,
                                                   GstCaps *caps,
                                                   GstCaps *othercaps);

static gboolean gst_rga_video_convert_set_info(GstVideoFilter *filter,
                                               GstCaps *incaps,
                                               GstVideoInfo *in_info,
                                               GstCaps *outcaps,
                                               GstVideoInfo *out_info);

static GstFlowReturn gst_rga_video_convert_transform_frame(
    GstVideoFilter *filter, GstVideoFrame *inframe, GstVideoFrame *outframe);

/* pad templates */

/* RGA3 src does not support planar YUV (I420/YV12/Y42B) or GRAY8.
 * We advertise the RGA2Enhanced superset here; if RGA3 is selected at
 * runtime and an unsupported format is negotiated, improcess will error. */
#define RGA_FORMATS \
  "{ RGBA, BGRA, ARGB, ABGR, RGBx, BGRx, xRGB, xBGR, " \
  "RGB, BGR, RGB16, " \
  "NV12, NV21, NV16, NV61, " \
  "I420, YV12, Y42B, " \
  "YUY2, YVYU, UYVY, " \
  "GRAY8 }"

#define VIDEO_SRC_CAPS \
  "video/x-raw, " \
  "format = (string) " RGA_FORMATS ", " \
  "width = (int) [ 2, 4096 ], " \
  "height = (int) [ 2, 4096 ], " \
  "framerate = (fraction) [ 0, max ]"

#define VIDEO_SINK_CAPS \
  "video/x-raw, " \
  "format = (string) " RGA_FORMATS ", " \
  "width = (int) [ 2, 8192 ], " \
  "height = (int) [ 2, 8192 ], " \
  "framerate = (fraction) [ 0, max ]"

/* element properties */

typedef enum {
  GST_RGA_PROP_0,
  GST_RGA_PROP_CORE_MASK,
  GST_RGA_PROP_FLIP,
  GST_RGA_PROP_ROTATION,
  GST_RGA_PROP_LAST
} GstRgaProp;

static GParamSpec *rga_props[GST_RGA_PROP_LAST];

/* class initialization */

G_DEFINE_TYPE_WITH_CODE(
    GstRgaVideoConvert, gst_rga_video_convert, GST_TYPE_VIDEO_FILTER,
    GST_DEBUG_CATEGORY_INIT(gst_rga_video_convert_debug_category,
                            "rgavideoconvert", 0,
                            "video Colorspace conversion & scaler"));

static void gst_rga_video_convert_set_property(GObject *object, guint prop_id,
                                               const GValue *value,
                                               GParamSpec *pspec);

static void gst_rga_video_convert_get_property(GObject *object, guint prop_id,
                                               GValue *value,
                                               GParamSpec *pspec);

static void gst_rga_video_convert_class_init(GstRgaVideoConvertClass *klass) {
  GObjectClass *gobject_class = G_OBJECT_CLASS(klass);
  GstBaseTransformClass *base_transform_class = GST_BASE_TRANSFORM_CLASS(klass);
  GstVideoFilterClass *video_filter_class = GST_VIDEO_FILTER_CLASS(klass);

  /* Setting up pads and setting metadata should be moved to
   base_class_init if you intend to subclass this class. */
  gst_element_class_add_pad_template(
      GST_ELEMENT_CLASS(klass),
      gst_pad_template_new("src", GST_PAD_SRC, GST_PAD_ALWAYS,
                           gst_caps_from_string(VIDEO_SRC_CAPS)));
  gst_element_class_add_pad_template(
      GST_ELEMENT_CLASS(klass),
      gst_pad_template_new("sink", GST_PAD_SINK, GST_PAD_ALWAYS,
                           gst_caps_from_string(VIDEO_SINK_CAPS)));

  gst_element_class_set_static_metadata(
      GST_ELEMENT_CLASS(klass), "RgaVidConv Plugin", "Generic",
      "Converts video from one colorspace to another & Resizes via Rockchip "
      "RGA",
      "http://github.com/corenel/gstreamer-rga");

  /* element properties */
  static const GFlagsValue mask_values[] = {
      {IM_SCHEDULER_RGA3_DEFAULT, "auto", "auto"},
      {IM_SCHEDULER_RGA3_CORE0, "rga3_core0", "rga3_core0"},
      {IM_SCHEDULER_RGA3_CORE1, "rga3_core1", "rga3_core1"},
      {IM_SCHEDULER_RGA2_CORE0, "rga2_core0", "rga2_core0"},
      {IM_SCHEDULER_RGA3_CORE0 | IM_SCHEDULER_RGA3_CORE1, "rga3", "rga3"},
      {IM_SCHEDULER_RGA2_CORE0, "rga2", "rga2"},
      {0, NULL, NULL}};
  GType mask_type = g_flags_register_static("GstRgaCoreMask", mask_values);

  rga_props[GST_RGA_PROP_CORE_MASK] = g_param_spec_flags(
      "core-mask", "Core mask", "Select which RGA core(s) to use (bit-mask)",
      mask_type, IM_SCHEDULER_RGA3_CORE0 | IM_SCHEDULER_RGA3_CORE1,
      G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

  static const GEnumValue flip_values[] = {
      {0, "none", "none"},
      {IM_HAL_TRANSFORM_FLIP_H, "horizontal", "horizontal"},
      {IM_HAL_TRANSFORM_FLIP_V, "vertical", "vertical"},
      {IM_HAL_TRANSFORM_FLIP_H_V, "both", "both"},
      {0, NULL, NULL}};
  GType flip_type = g_enum_register_static("GstRgaFlip", flip_values);
  rga_props[GST_RGA_PROP_FLIP] = g_param_spec_enum(
      "flip", "Flip", "Flip the image (none/horizontal/vertical/both)",
      flip_type, 0, G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

  static const GEnumValue rotation_values[] = {
      {0, "none", "none"},
      {IM_HAL_TRANSFORM_ROT_90, "90", "90"},
      {IM_HAL_TRANSFORM_ROT_180, "180", "180"},
      {IM_HAL_TRANSFORM_ROT_270, "270", "270"},
      {0, NULL, NULL}};
  GType rotation_type = g_enum_register_static("GstRgaRotation", rotation_values);
  rga_props[GST_RGA_PROP_ROTATION] = g_param_spec_enum(
      "rotation", "Rotation", "Rotate the image (none/90/180/270 degrees)",
      rotation_type, 0, G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

  gobject_class->set_property = gst_rga_video_convert_set_property;
  gobject_class->get_property = gst_rga_video_convert_get_property;
  g_object_class_install_property(gobject_class, GST_RGA_PROP_CORE_MASK,
                                  rga_props[GST_RGA_PROP_CORE_MASK]);
  g_object_class_install_property(gobject_class, GST_RGA_PROP_FLIP,
                                  rga_props[GST_RGA_PROP_FLIP]);
  g_object_class_install_property(gobject_class, GST_RGA_PROP_ROTATION,
                                  rga_props[GST_RGA_PROP_ROTATION]);

  base_transform_class->transform_caps =
      GST_DEBUG_FUNCPTR(gst_rga_video_convert_transform_caps);

  base_transform_class->fixate_caps =
      GST_DEBUG_FUNCPTR(gst_rga_video_convert_fixate_caps);
  base_transform_class->start = GST_DEBUG_FUNCPTR(gst_rga_video_convert_start);
  base_transform_class->stop = GST_DEBUG_FUNCPTR(gst_rga_video_convert_stop);
  video_filter_class->set_info =
      GST_DEBUG_FUNCPTR(gst_rga_video_convert_set_info);
  video_filter_class->transform_frame =
      GST_DEBUG_FUNCPTR(gst_rga_video_convert_transform_frame);
}

static GstCaps *gst_rga_video_convert_fixate_caps(GstBaseTransform *trans,
                                                   GstPadDirection direction,
                                                   GstCaps *caps,
                                                   GstCaps *othercaps) {
  GstStructure *ins = gst_caps_get_structure(caps, 0);
  GstCaps *result = gst_caps_make_writable(othercaps);
  GstStructure *outs = gst_caps_get_structure(result, 0);
  gint w, h;
  const gchar *fmt;

  if (gst_structure_get_int(ins, "width", &w))
    gst_structure_fixate_field_nearest_int(outs, "width", w);
  if (gst_structure_get_int(ins, "height", &h))
    gst_structure_fixate_field_nearest_int(outs, "height", h);
  if ((fmt = gst_structure_get_string(ins, "format")))
    gst_structure_fixate_field_string(outs, "format", fmt);

  return gst_caps_fixate(result);
}

static RgaSURF_FORMAT gst_gst_format_to_rga_format(GstVideoFormat format) {
  switch (format) {
    /* 32-bit RGBA variants */
    GST_CASE_RETURN(GST_VIDEO_FORMAT_RGBA, RK_FORMAT_RGBA_8888);
    GST_CASE_RETURN(GST_VIDEO_FORMAT_BGRA, RK_FORMAT_BGRA_8888);
    GST_CASE_RETURN(GST_VIDEO_FORMAT_ARGB, RK_FORMAT_ARGB_8888);
    GST_CASE_RETURN(GST_VIDEO_FORMAT_ABGR, RK_FORMAT_ABGR_8888);
    GST_CASE_RETURN(GST_VIDEO_FORMAT_RGBx, RK_FORMAT_RGBX_8888);
    GST_CASE_RETURN(GST_VIDEO_FORMAT_BGRx, RK_FORMAT_BGRX_8888);
    GST_CASE_RETURN(GST_VIDEO_FORMAT_xRGB, RK_FORMAT_XRGB_8888);
    GST_CASE_RETURN(GST_VIDEO_FORMAT_xBGR, RK_FORMAT_XBGR_8888);
    /* 24-bit RGB */
    GST_CASE_RETURN(GST_VIDEO_FORMAT_RGB, RK_FORMAT_RGB_888);
    GST_CASE_RETURN(GST_VIDEO_FORMAT_BGR, RK_FORMAT_BGR_888);
    /* 16-bit RGB */
    GST_CASE_RETURN(GST_VIDEO_FORMAT_RGB16, RK_FORMAT_RGB_565);
    /* YUV semi-planar */
    GST_CASE_RETURN(GST_VIDEO_FORMAT_NV12, RK_FORMAT_YCbCr_420_SP);
    GST_CASE_RETURN(GST_VIDEO_FORMAT_NV21, RK_FORMAT_YCrCb_420_SP);
    GST_CASE_RETURN(GST_VIDEO_FORMAT_NV16, RK_FORMAT_YCbCr_422_SP);
    GST_CASE_RETURN(GST_VIDEO_FORMAT_NV61, RK_FORMAT_YCrCb_422_SP);
#ifdef HAVE_NV12_10LE40
    GST_CASE_RETURN(GST_VIDEO_FORMAT_NV12_10LE40, RK_FORMAT_YCbCr_420_SP_10B);
#endif
    /* YUV planar (RGA2Enhanced only) */
    GST_CASE_RETURN(GST_VIDEO_FORMAT_I420, RK_FORMAT_YCbCr_420_P);
    GST_CASE_RETURN(GST_VIDEO_FORMAT_YV12, RK_FORMAT_YCrCb_420_P);
    GST_CASE_RETURN(GST_VIDEO_FORMAT_Y42B, RK_FORMAT_YCbCr_422_P);
    /* YUV packed */
    GST_CASE_RETURN(GST_VIDEO_FORMAT_YUY2, RK_FORMAT_YUYV_422);
    GST_CASE_RETURN(GST_VIDEO_FORMAT_YVYU, RK_FORMAT_YVYU_422);
    GST_CASE_RETURN(GST_VIDEO_FORMAT_UYVY, RK_FORMAT_UYVY_422);
    /* Grayscale (RGA2Enhanced only) */
    GST_CASE_RETURN(GST_VIDEO_FORMAT_GRAY8, RK_FORMAT_YCbCr_400);
    default:
      return RK_FORMAT_UNKNOWN;
  }
}

static rga_buffer_t gst_rga_buffer_from_video_frame(GstVideoFrame *frame,
                                                     GstMapInfo *map_info,
                                                     GstMapFlags map_flags) {
  rga_buffer_t buf = {0};

  RgaSURF_FORMAT format =
      gst_gst_format_to_rga_format(GST_VIDEO_FRAME_FORMAT(frame));
  guint width = GST_VIDEO_FRAME_WIDTH(frame);
  guint height = GST_VIDEO_FRAME_HEIGHT(frame);
  guint hstride = GST_VIDEO_FRAME_PLANE_STRIDE(frame, 0);
  guint vstride = GST_VIDEO_FRAME_N_PLANES(frame) == 1
                      ? GST_VIDEO_INFO_HEIGHT(&frame->info)
                      : GST_VIDEO_INFO_PLANE_OFFSET(&frame->info, 1) / hstride;

  gint pixel_stride;

  switch (format) {
    case RK_FORMAT_RGBX_8888:
    case RK_FORMAT_BGRX_8888:
    case RK_FORMAT_RGBA_8888:
    case RK_FORMAT_BGRA_8888:
      pixel_stride = 4;
      break;
    case RK_FORMAT_RGB_888:
    case RK_FORMAT_BGR_888:
      pixel_stride = 3;
      break;
    case RK_FORMAT_RGBA_5551:
    case RK_FORMAT_RGB_565:
      pixel_stride = 2;
      break;
    default:
      pixel_stride = 1;

      /* RGA requires yuv image rect align to 2 */
      width &= ~1;
      height &= ~1;
      break;
  }

  if (hstride / pixel_stride >= width) hstride /= pixel_stride;

  buf.width = width;
  buf.height = height;
  buf.wstride = hstride;
  buf.hstride = vstride;
  buf.format = format;

  GstBuffer *gbuf = frame->buffer;
  if (gst_buffer_n_memory(gbuf) == 1) {
    GstMemory *mem = gst_buffer_peek_memory(gbuf, 0);
    if (gst_is_dmabuf_memory(mem)) {
      gsize offset;
      gst_memory_get_sizes(mem, &offset, NULL);
      if (!offset) buf.fd = gst_dmabuf_memory_get_fd(mem);
    }
  }

  if (buf.fd <= 0) {
    gst_buffer_map(gbuf, map_info, map_flags);
    buf.vir_addr = map_info->data;
  }
  return buf;
}

static GstCaps *gst_rga_video_convert_transform_caps(GstBaseTransform *trans,
                                                     GstPadDirection direction,
                                                     GstCaps *caps,
                                                     GstCaps *filter) {
  GST_DEBUG_OBJECT(trans,
                   "transform direction %s : caps=%" GST_PTR_FORMAT
                   "    filter=%" GST_PTR_FORMAT,
                   direction == GST_PAD_SINK ? "sink" : "src", caps, filter);

  GstCaps *ret;
  GstStructure *structure;
  GstCapsFeatures *features;
  gint i, n;

  ret = gst_caps_new_empty();
  n = gst_caps_get_size(caps);
  for (i = 0; i < n; i++) {
    structure = gst_caps_get_structure(caps, i);
    features = gst_caps_get_features(caps, i);

    /* If this is already expressed by the existing caps
     * skip this structure */
    if (i > 0 && gst_caps_is_subset_structure_full(ret, structure, features))
      continue;

    /* make copy */
    structure = gst_structure_copy(structure);

    if (direction == GST_PAD_SRC) {
      // rga 输出最大 4096
      gst_structure_set(structure, "width", GST_TYPE_INT_RANGE, 2, 4096,
                        "height", GST_TYPE_INT_RANGE, 2, 4096, NULL);
    } else {
      // 输入最大 8192
      gst_structure_set(structure, "width", GST_TYPE_INT_RANGE, 2, 8192,
                        "height", GST_TYPE_INT_RANGE, 2, 8192, NULL);
    }
    if (!gst_caps_features_is_any(features)) {
      gst_structure_remove_fields(structure, "format", "colorimetry",
                                  "chroma-site", NULL);
    }

    gst_caps_append_structure_full(ret, structure,
                                   gst_caps_features_copy(features));
  }

  if (filter) {
    GstCaps *intersection;

    intersection =
        gst_caps_intersect_full(filter, ret, GST_CAPS_INTERSECT_FIRST);
    gst_caps_unref(ret);
    ret = intersection;
  }

  GST_DEBUG_OBJECT(trans, "returning caps: %" GST_PTR_FORMAT, ret);

  return ret;
}

static void gst_rga_video_convert_set_property(GObject *object, guint prop_id,
                                               const GValue *value,
                                               GParamSpec *pspec) {
  GstRgaVideoConvert *rgavideoconvert = gst_rga_video_convert(object);
  switch (prop_id) {
    case GST_RGA_PROP_CORE_MASK:
      rgavideoconvert->core_mask = g_value_get_flags(value);
      break;
    case GST_RGA_PROP_FLIP:
      rgavideoconvert->flip = g_value_get_enum(value);
      break;
    case GST_RGA_PROP_ROTATION:
      rgavideoconvert->rotation = g_value_get_enum(value);
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID(object, prop_id, pspec);
  }
}

static void gst_rga_video_convert_get_property(GObject *object, guint prop_id,
                                               GValue *value,
                                               GParamSpec *pspec) {
  GstRgaVideoConvert *rgavideoconvert = gst_rga_video_convert(object);
  switch (prop_id) {
    case GST_RGA_PROP_CORE_MASK:
      g_value_set_flags(value, rgavideoconvert->core_mask);
      break;
    case GST_RGA_PROP_FLIP:
      g_value_set_enum(value, rgavideoconvert->flip);
      break;
    case GST_RGA_PROP_ROTATION:
      g_value_set_enum(value, rgavideoconvert->rotation);
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID(object, prop_id, pspec);
  }
}

static void gst_rga_video_convert_init(GstRgaVideoConvert *rgavideoconvert) {}

static gboolean gst_rga_video_convert_start(GstBaseTransform *trans) {
  GstRgaVideoConvert *rgavideoconvert = gst_rga_video_convert(trans);

  GST_DEBUG_OBJECT(rgavideoconvert, "start");
  c_RkRgaInit();
  if (rgavideoconvert->core_mask) {
    imconfig(IM_CONFIG_SCHEDULER_CORE, rgavideoconvert->core_mask);
  }
  return TRUE;
}

static gboolean gst_rga_video_convert_stop(GstBaseTransform *trans) {
  GstRgaVideoConvert *rgavideoconvert = gst_rga_video_convert(trans);
  GST_DEBUG_OBJECT(rgavideoconvert, "stop");
  c_RkRgaDeInit();
  return TRUE;
}

static gboolean gst_rga_video_convert_set_info(GstVideoFilter *filter,
                                               GstCaps *incaps,
                                               GstVideoInfo *in_info,
                                               GstCaps *outcaps,
                                               GstVideoInfo *out_info) {
  GstRgaVideoConvert *rgavideoconvert = gst_rga_video_convert(filter);
  GST_DEBUG_OBJECT(rgavideoconvert, "set_info");

  GstVideoFormat in_format = GST_VIDEO_INFO_FORMAT(in_info);
  GstVideoFormat out_format = GST_VIDEO_INFO_FORMAT(out_info);

  if (gst_gst_format_to_rga_format(in_format) == RK_FORMAT_UNKNOWN ||
      gst_gst_format_to_rga_format(out_format) == RK_FORMAT_UNKNOWN) {
    GST_INFO_OBJECT(rgavideoconvert, "don't support format. in format=%d,out format=%d",
                    in_format, out_format);
    return FALSE;
  }
  return TRUE;
}

/* transform */
static GstFlowReturn gst_rga_video_convert_transform_frame(
    GstVideoFilter *filter, GstVideoFrame *inframe, GstVideoFrame *outframe) {
  GstRgaVideoConvert *rgavideoconvert = gst_rga_video_convert(filter);

  GST_DEBUG_OBJECT(rgavideoconvert, "transform_frame");

  GstMapInfo in_map = {0};
  GstMapInfo out_map = {0};

  rga_buffer_t src = gst_rga_buffer_from_video_frame(inframe, &in_map, GST_MAP_READ);
  rga_buffer_t dst = gst_rga_buffer_from_video_frame(outframe, &out_map, GST_MAP_WRITE);

  if (rgavideoconvert->core_mask)
    imconfig(IM_CONFIG_SCHEDULER_CORE, rgavideoconvert->core_mask);

  int usage = rgavideoconvert->flip | rgavideoconvert->rotation;
  im_rect empty = {0};
  IM_STATUS status = improcess(src, dst, (rga_buffer_t){0}, empty, empty, empty, usage);

  gst_buffer_unmap(inframe->buffer, &in_map);
  gst_buffer_unmap(outframe->buffer, &out_map);

  if (status != IM_STATUS_SUCCESS) {
    GST_WARNING_OBJECT(filter, "improcess failed: %s", imStrError(status));
    return GST_FLOW_ERROR;
  }

  return GST_FLOW_OK;
}

static gboolean plugin_init(GstPlugin *plugin) {
  /* FIXME Remember to set the rank if it's an element that is meant
   to be autoplugged by decodebin. */
  return gst_element_register(plugin, "rgavideoconvert", GST_RANK_PRIMARY,
                              GST_TYPE_RGA_VIDEO_CONVERT);
}

#ifndef VERSION
#define VERSION "1.0.0"
#endif
#ifndef PACKAGE
#define PACKAGE "gstreamer-rga"
#endif
#ifndef PACKAGE_NAME
#define PACKAGE_NAME "gstreamer-rga"
#endif
#ifndef GST_PACKAGE_ORIGIN
#define GST_PACKAGE_ORIGIN "https://github.com/corenel/gstreamer-rga.git"
#endif

GST_PLUGIN_DEFINE(GST_VERSION_MAJOR, GST_VERSION_MINOR, rgavideoconvert,
                  "video Colorspace conversion & scaler", plugin_init, VERSION,
                  "MIT/X11", PACKAGE_NAME, GST_PACKAGE_ORIGIN)
