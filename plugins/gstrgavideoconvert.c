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
 *
 * Both pads also accept the "memory:DMABuf" caps feature. When upstream hands
 * us DMABuf backed buffers the file descriptor is imported into RGA directly,
 * so no CPU mapping of the frame takes place:
 * |[
 * gst-launch-1.0 -v filesrc location=in.mp4 ! parsebin ! mppvideodec !
 * video/x-raw\(memory:DMABuf\) ! rgavideoconvert !
 * video/x-raw,format=RGBA,width=640,height=480 ! fakesink
 * ]|
 * </refsect2>
 */

#include "gst/gstpluginfeature.h"
#ifdef HAVE_CONFIG_H
#include "config.h"  // NOLINT
#endif

#include <gst/allocators/gstdmabuf.h>
#include <gst/gst.h>
#include <gst/video/gstvideofilter.h>
#include <gst/video/gstvideopool.h>
#include <gst/video/video.h>

#include "gstrgavideoconvert.h"  // NOLINT
#include "rga/RgaApi.h"
#include "rga/im2d.h"

#ifndef GST_CAPS_FEATURE_MEMORY_DMABUF
#define GST_CAPS_FEATURE_MEMORY_DMABUF "memory:DMABuf"
#endif

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

static gboolean gst_rga_video_convert_propose_allocation(
    GstBaseTransform *trans, GstQuery *decide_query, GstQuery *query);

static gboolean gst_rga_video_convert_decide_allocation(GstBaseTransform *trans,
                                                        GstQuery *query);

static gboolean gst_rga_video_convert_set_info(GstVideoFilter *filter,
                                               GstCaps *incaps,
                                               GstVideoInfo *in_info,
                                               GstCaps *outcaps,
                                               GstVideoInfo *out_info);

static GstFlowReturn gst_rga_video_convert_transform(GstBaseTransform *trans,
                                                     GstBuffer *inbuf,
                                                     GstBuffer *outbuf);

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

#define RGA_CAPS_FIELDS(max_width, max_height) \
  "format = (string) " RGA_FORMATS ", " \
  "width = (int) [ 2, " max_width " ], " \
  "height = (int) [ 2, " max_height " ], " \
  "framerate = (fraction) [ 0, max ]"

#define RGA_DMABUF_CAPS(max_width, max_height) \
  "video/x-raw(" GST_CAPS_FEATURE_MEMORY_DMABUF "), " \
  RGA_CAPS_FIELDS(max_width, max_height)

#define RGA_SYSMEM_CAPS(max_width, max_height) \
  "video/x-raw, " RGA_CAPS_FIELDS(max_width, max_height)

/* System memory comes first on the src pad: RGA renders into memory that
 * somebody else allocated and we cannot export a DMABuf ourselves, so DMABuf
 * output only works when downstream provides a DMABuf pool. Downstream that
 * wants DMABufs still gets them, its own caps order wins during negotiation. 
 * On output prefer system memory since if a downstream element has ANY memory requirement
 * it does not care what input it gets and will not allocate no buffer for us.
 * So the only memory we can allocate is system memory. */
#define VIDEO_SRC_CAPS \
  RGA_SYSMEM_CAPS("4096", "4096") "; " RGA_DMABUF_CAPS("4096", "4096")

/* DMABuf comes first on the sink pad: importing an fd is always cheaper than
 * mapping the frame for the CPU. Prefer dma on input and negotiate for 
 * it if the upstream element provides it. */
#define VIDEO_SINK_CAPS \
  RGA_DMABUF_CAPS("8192", "8192") "; " RGA_SYSMEM_CAPS("8192", "8192")

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
  /* "auto" is 0: no core is requested and imconfig() is not called at all, so
   * librga's own scheduler decides. Note that IM_SCHEDULER_RGA3_DEFAULT is an
   * alias for RGA3_CORE0, which is not the same thing. */
  static const GFlagsValue mask_values[] = {
      {0, "auto", "auto"},
      {IM_SCHEDULER_RGA3_CORE0, "rga3_core0", "rga3_core0"},
      {IM_SCHEDULER_RGA3_CORE1, "rga3_core1", "rga3_core1"},
      {IM_SCHEDULER_RGA2_CORE0, "rga2_core0", "rga2_core0"},
      {IM_SCHEDULER_RGA3_CORE0 | IM_SCHEDULER_RGA3_CORE1, "rga3", "rga3"},
      {IM_SCHEDULER_RGA2_CORE0, "rga2", "rga2"},
      {0, NULL, NULL}};
  GType mask_type = g_flags_register_static("GstRgaCoreMask", mask_values);

  /* Default to "auto" rather than pinning to RGA3: parts like the RK356x have
   * no RGA3 core at all, and requesting one there would fail every job. */
  rga_props[GST_RGA_PROP_CORE_MASK] = g_param_spec_flags(
      "core-mask", "Core mask", "Select which RGA core(s) to use (bit-mask)",
      mask_type, 0, G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

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
  base_transform_class->propose_allocation =
      GST_DEBUG_FUNCPTR(gst_rga_video_convert_propose_allocation);
  base_transform_class->decide_allocation =
      GST_DEBUG_FUNCPTR(gst_rga_video_convert_decide_allocation);
  base_transform_class->start = GST_DEBUG_FUNCPTR(gst_rga_video_convert_start);
  base_transform_class->stop = GST_DEBUG_FUNCPTR(gst_rga_video_convert_stop);
  /* transform() takes the zero-copy path when a DMABuf is involved and falls
   * back to GstVideoFilter's mapped transform_frame() otherwise. */
  base_transform_class->transform =
      GST_DEBUG_FUNCPTR(gst_rga_video_convert_transform);
  video_filter_class->set_info =
      GST_DEBUG_FUNCPTR(gst_rga_video_convert_set_info);
  video_filter_class->transform_frame =
      GST_DEBUG_FUNCPTR(gst_rga_video_convert_transform_frame);
}

/* 90 and 270 degree rotations transpose the image, so the two pads disagree
 * about which dimension is the width. */
static gboolean gst_rga_rotation_transposes(guint32 rotation) {
  return rotation == IM_HAL_TRANSFORM_ROT_90 ||
         rotation == IM_HAL_TRANSFORM_ROT_270;
}

static gboolean gst_rga_video_convert_transposes(
    GstRgaVideoConvert *rgavideoconvert) {
  gboolean transposes;

  GST_OBJECT_LOCK(rgavideoconvert);
  transposes = gst_rga_rotation_transposes(rgavideoconvert->rotation);
  GST_OBJECT_UNLOCK(rgavideoconvert);

  return transposes;
}

static GstCaps *gst_rga_video_convert_fixate_caps(GstBaseTransform *trans,
                                                   GstPadDirection direction,
                                                   GstCaps *caps,
                                                   GstCaps *othercaps) {
  GstRgaVideoConvert *rgavideoconvert = gst_rga_video_convert(trans);
  GstStructure *ins = gst_caps_get_structure(caps, 0);
  GstCaps *result = gst_caps_make_writable(othercaps);
  GstStructure *outs = gst_caps_get_structure(result, 0);
  gboolean have_w, have_h;
  gint w, h;
  const gchar *fmt;

  have_w = gst_structure_get_int(ins, "width", &w);
  have_h = gst_structure_get_int(ins, "height", &h);

  /* Default to keeping the size, transposed when we rotate by 90/270. Swapping
   * is its own inverse, so the same swap is right in both directions. */
  if (gst_rga_video_convert_transposes(rgavideoconvert)) {
    gboolean had_w = have_w;
    gint old_w = w;

    have_w = have_h;
    w = h;
    have_h = had_w;
    h = old_w;
  }

  if (have_w) gst_structure_fixate_field_nearest_int(outs, "width", w);
  if (have_h) gst_structure_fixate_field_nearest_int(outs, "height", h);
  if ((fmt = gst_structure_get_string(ins, "format")))
    gst_structure_fixate_field_string(outs, "format", fmt);

  GST_DEBUG_OBJECT(trans, "fixated to %" GST_PTR_FORMAT, result);

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

/* Fills in the geometry of @buf from @info. wrapbuffer_fd() and
 * wrapbuffer_virtualaddr() assume unpadded buffers, so the strides always have
 * to be corrected afterwards. */
static void gst_rga_buffer_set_geometry(rga_buffer_t *buf,
                                        const GstVideoInfo *info) {
  RgaSURF_FORMAT format =
      gst_gst_format_to_rga_format(GST_VIDEO_INFO_FORMAT(info));
  guint width = GST_VIDEO_INFO_WIDTH(info);
  guint height = GST_VIDEO_INFO_HEIGHT(info);
  guint hstride = GST_VIDEO_INFO_PLANE_STRIDE(info, 0);
  guint vstride = GST_VIDEO_INFO_N_PLANES(info) == 1
                      ? GST_VIDEO_INFO_HEIGHT(info)
                      : GST_VIDEO_INFO_PLANE_OFFSET(info, 1) / hstride;

  gint pixel_stride;

  /* RGA counts wstride in pixels, GStreamer in bytes, so the byte stride has to
   * be divided by the size of a pixel. Every format the pads advertise needs an
   * entry here - a missing one lands in the default and describes the surface
   * as pixel_stride times too wide, which the driver rejects. */
  switch (format) {
    case RK_FORMAT_RGBA_8888:
    case RK_FORMAT_BGRA_8888:
    case RK_FORMAT_ARGB_8888:
    case RK_FORMAT_ABGR_8888:
    case RK_FORMAT_RGBX_8888:
    case RK_FORMAT_BGRX_8888:
    case RK_FORMAT_XRGB_8888:
    case RK_FORMAT_XBGR_8888:
      pixel_stride = 4;
      break;
    case RK_FORMAT_RGB_888:
    case RK_FORMAT_BGR_888:
      pixel_stride = 3;
      break;
    case RK_FORMAT_RGBA_5551:
    case RK_FORMAT_RGB_565:
    /* Packed 4:2:2 is two bytes per pixel as well. */
    case RK_FORMAT_YUYV_422:
    case RK_FORMAT_YVYU_422:
    case RK_FORMAT_UYVY_422:
      pixel_stride = 2;
      break;
    default:
      /* planar and semi-planar YUV, and grayscale: one byte per sample */
      pixel_stride = 1;
      break;
  }

  /* RGA requires yuv image rect align to 2 */
  if (GST_VIDEO_INFO_IS_YUV(info) || GST_VIDEO_INFO_IS_GRAY(info)) {
    width &= ~1;
    height &= ~1;
  }

  if (hstride / pixel_stride >= width) hstride /= pixel_stride;

  buf->width = width;
  buf->height = height;
  buf->wstride = hstride;
  buf->hstride = vstride;
  buf->format = format;

  GST_LOG("%s %ux%u, wstride %u px, hstride %u, rga format 0x%x",
          GST_VIDEO_INFO_NAME(info), width, height, hstride, vstride, format);
}

/* Returns the DMABuf file descriptor backing @buffer, or -1 when @buffer cannot
 * be handed to RGA as a DMABuf. RGA imports a whole fd, so the buffer has to be
 * backed by a single dmabuf memory starting at offset 0. */
static gint gst_rga_buffer_get_dmabuf_fd(GstBuffer *buffer) {
  GstMemory *mem;
  gsize offset;

  if (gst_buffer_n_memory(buffer) != 1) return -1;

  mem = gst_buffer_peek_memory(buffer, 0);
  if (!gst_is_dmabuf_memory(mem)) return -1;

  gst_memory_get_sizes(mem, &offset, NULL);
  if (offset != 0) return -1;

  return gst_dmabuf_memory_get_fd(mem);
}

/* gst_video_frame_map() applies the GstVideoMeta for us; DMABufs are never
 * mapped, so their layout has to be picked up by hand. */
static void gst_rga_video_info_from_buffer(const GstVideoInfo *ref,
                                           GstBuffer *buffer,
                                           GstVideoInfo *info) {
  GstVideoMeta *meta = gst_buffer_get_video_meta(buffer);
  guint i;

  *info = *ref;
  if (meta == NULL) return;

  GST_VIDEO_INFO_WIDTH(info) = meta->width;
  GST_VIDEO_INFO_HEIGHT(info) = meta->height;
  for (i = 0; i < meta->n_planes && i < GST_VIDEO_MAX_PLANES; i++) {
    GST_VIDEO_INFO_PLANE_OFFSET(info, i) = meta->offset[i];
    GST_VIDEO_INFO_PLANE_STRIDE(info, i) = meta->stride[i];
  }
}

static rga_buffer_t gst_rga_buffer_from_video_frame(GstVideoFrame *frame) {
  rga_buffer_t buf = wrapbuffer_virtualaddr(
      GST_VIDEO_FRAME_PLANE_DATA(frame, 0), GST_VIDEO_FRAME_WIDTH(frame),
      GST_VIDEO_FRAME_HEIGHT(frame),
      gst_gst_format_to_rga_format(GST_VIDEO_FRAME_FORMAT(frame)));

  gst_rga_buffer_set_geometry(&buf, &frame->info);
  return buf;
}

/* One side of the conversion as librga sees it, plus the CPU mapping that had
 * to be taken when the buffer could not be imported as a DMABuf. */
typedef struct {
  rga_buffer_t buf;
  GstVideoFrame frame;
  gboolean mapped;
} GstRgaSurface;

static gboolean gst_rga_surface_open(GstRgaVideoConvert *rgavideoconvert,
                                     GstBuffer *buffer,
                                     const GstVideoInfo *info,
                                     GstMapFlags map_flags,
                                     GstRgaSurface *surface) {
  gint fd;

  *surface = (GstRgaSurface){0};

  fd = gst_rga_buffer_get_dmabuf_fd(buffer);
  if (fd >= 0) {
    GstVideoInfo dmabuf_info;

    gst_rga_video_info_from_buffer(info, buffer, &dmabuf_info);
    surface->buf = wrapbuffer_fd(
        fd, GST_VIDEO_INFO_WIDTH(&dmabuf_info),
        GST_VIDEO_INFO_HEIGHT(&dmabuf_info),
        gst_gst_format_to_rga_format(GST_VIDEO_INFO_FORMAT(&dmabuf_info)));
    gst_rga_buffer_set_geometry(&surface->buf, &dmabuf_info);

    GST_LOG_OBJECT(rgavideoconvert, "imported dmabuf fd %d", fd);
    return TRUE;
  }

  if (!gst_video_frame_map(&surface->frame, (GstVideoInfo *)info, buffer,
                           map_flags)) {
    GST_ERROR_OBJECT(rgavideoconvert, "failed to map buffer %" GST_PTR_FORMAT,
                     buffer);
    return FALSE;
  }
  surface->mapped = TRUE;
  surface->buf = gst_rga_buffer_from_video_frame(&surface->frame);
  return TRUE;
}

static void gst_rga_surface_close(GstRgaSurface *surface) {
  if (surface->mapped) gst_video_frame_unmap(&surface->frame);
  surface->mapped = FALSE;
}

static GstFlowReturn gst_rga_video_convert_run(
    GstRgaVideoConvert *rgavideoconvert, rga_buffer_t *src, rga_buffer_t *dst) {
  im_rect empty = {0};
  IM_STATUS status;
  guint32 core_mask;
  int usage;

  /* Snapshot the properties, then let go of the lock before entering librga.
   * active_rotation rather than rotation: the buffers were sized for the angle
   * the caps were negotiated with. */
  GST_OBJECT_LOCK(rgavideoconvert);
  core_mask = rgavideoconvert->core_mask;
  usage = rgavideoconvert->flip | rgavideoconvert->active_rotation;
  GST_OBJECT_UNLOCK(rgavideoconvert);

  if (core_mask) imconfig(IM_CONFIG_SCHEDULER_CORE, core_mask);

  status =
      improcess(*src, *dst, (rga_buffer_t){0}, empty, empty, empty, usage);
  if (status != IM_STATUS_SUCCESS) {
    GST_WARNING_OBJECT(rgavideoconvert, "improcess failed: %s",
                       imStrError(status));
    return GST_FLOW_ERROR;
  }
  return GST_FLOW_OK;
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
  gboolean transposes =
      gst_rga_video_convert_transposes(gst_rga_video_convert(trans));

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

    /* The ranges above are square, so transposing does not change them - we
     * scale anyway, any size in range is on offer. Non-square pixels do become
     * their own inverse though, and fixate_caps() swaps the dimensions once a
     * concrete size is picked. */
    if (transposes) {
      gint par_n, par_d;

      if (gst_structure_get_fraction(structure, "pixel-aspect-ratio", &par_n,
                                     &par_d) &&
          par_n > 0 && par_d > 0)
        gst_structure_set(structure, "pixel-aspect-ratio", GST_TYPE_FRACTION,
                          par_d, par_n, NULL);
    }

    if (gst_caps_features_is_any(features)) {
      gst_caps_append_structure_full(ret, structure,
                                     gst_caps_features_copy(features));
      continue;
    }

    gst_structure_remove_fields(structure, "format", "colorimetry",
                                "chroma-site", NULL);

    /* The memory type of the two pads is independent: RGA can read from a
     * DMABuf and write into system memory and the other way round. Offer both
     * for every structure, most preferred first. */
    if (gst_caps_features_contains(features, GST_CAPS_FEATURE_MEMORY_DMABUF) ||
        gst_caps_features_is_equal(features,
                                   GST_CAPS_FEATURES_MEMORY_SYSTEM_MEMORY)) {
      GstStructure *alternate = gst_structure_copy(structure);

      if (direction == GST_PAD_SRC) {
        /* transforming to the sink pad: importing a DMABuf beats mapping */
        gst_caps_append_structure_full(
            ret, structure,
            gst_caps_features_new(GST_CAPS_FEATURE_MEMORY_DMABUF, NULL));
        gst_caps_append_structure_full(ret, alternate,
                                       gst_caps_features_new_empty());
      } else {
        /* transforming to the src pad: we cannot allocate DMABufs ourselves */
        gst_caps_append_structure_full(ret, structure,
                                       gst_caps_features_new_empty());
        gst_caps_append_structure_full(
            ret, alternate,
            gst_caps_features_new(GST_CAPS_FEATURE_MEMORY_DMABUF, NULL));
      }
      continue;
    }

    /* some other memory type, leave it to the caps intersection below */
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
      GST_OBJECT_LOCK(rgavideoconvert);
      rgavideoconvert->core_mask = g_value_get_flags(value);
      GST_OBJECT_UNLOCK(rgavideoconvert);
      break;
    case GST_RGA_PROP_FLIP:
      GST_OBJECT_LOCK(rgavideoconvert);
      rgavideoconvert->flip = g_value_get_enum(value);
      GST_OBJECT_UNLOCK(rgavideoconvert);
      break;
    case GST_RGA_PROP_ROTATION: {
      guint32 rotation = g_value_get_enum(value);
      gboolean renegotiate;

      GST_OBJECT_LOCK(rgavideoconvert);
      renegotiate = gst_rga_rotation_transposes(rotation) !=
                    gst_rga_rotation_transposes(rgavideoconvert->rotation);
      rgavideoconvert->rotation = rotation;
      /* When the transposition is unchanged the negotiated caps stay valid, so
       * the new angle can be used from the next frame on. Otherwise set_info()
       * latches it once the renegotiated caps are in place. */
      if (!renegotiate) rgavideoconvert->active_rotation = rotation;
      GST_OBJECT_UNLOCK(rgavideoconvert);

      if (renegotiate) {
        GST_DEBUG_OBJECT(rgavideoconvert,
                         "rotation changed to %u, renegotiating caps", rotation);
        gst_base_transform_reconfigure_src(GST_BASE_TRANSFORM(rgavideoconvert));
      }
      break;
    }
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID(object, prop_id, pspec);
  }
}

static void gst_rga_video_convert_get_property(GObject *object, guint prop_id,
                                               GValue *value,
                                               GParamSpec *pspec) {
  GstRgaVideoConvert *rgavideoconvert = gst_rga_video_convert(object);

  GST_OBJECT_LOCK(rgavideoconvert);
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
  GST_OBJECT_UNLOCK(rgavideoconvert);
}

static void gst_rga_video_convert_init(GstRgaVideoConvert *rgavideoconvert) {}

static gboolean gst_rga_video_convert_start(GstBaseTransform *trans) {
  GstRgaVideoConvert *rgavideoconvert = gst_rga_video_convert(trans);
  guint32 core_mask;

  GST_DEBUG_OBJECT(rgavideoconvert, "start");
  c_RkRgaInit();

  GST_OBJECT_LOCK(rgavideoconvert);
  core_mask = rgavideoconvert->core_mask;
  GST_OBJECT_UNLOCK(rgavideoconvert);

  if (core_mask) imconfig(IM_CONFIG_SCHEDULER_CORE, core_mask);
  return TRUE;
}

static gboolean gst_rga_video_convert_stop(GstBaseTransform *trans) {
  GstRgaVideoConvert *rgavideoconvert = gst_rga_video_convert(trans);
  GST_DEBUG_OBJECT(rgavideoconvert, "stop");
  c_RkRgaDeInit();
  return TRUE;
}

static gboolean gst_rga_caps_have_dmabuf(GstCaps *caps) {
  guint i, n = gst_caps_get_size(caps);

  for (i = 0; i < n; i++) {
    GstCapsFeatures *features = gst_caps_get_features(caps, i);

    if (features != NULL &&
        gst_caps_features_contains(features, GST_CAPS_FEATURE_MEMORY_DMABUF))
      return TRUE;
  }
  return FALSE;
}

static gboolean gst_rga_video_convert_propose_allocation(
    GstBaseTransform *trans, GstQuery *decide_query, GstQuery *query) {
  GstRgaVideoConvert *rgavideoconvert = gst_rga_video_convert(trans);
  GstCaps *caps = NULL;

  if (!GST_BASE_TRANSFORM_CLASS(gst_rga_video_convert_parent_class)
           ->propose_allocation(trans, decide_query, query))
    return FALSE;

  /* in passthrough the query was simply forwarded downstream */
  if (decide_query == NULL) return TRUE;

  /* We take the strides and plane offsets from the video meta, so upstream is
   * free to hand us padded buffers. */
  if (!gst_query_find_allocation_meta(query, GST_VIDEO_META_API_TYPE, NULL))
    gst_query_add_allocation_meta(query, GST_VIDEO_META_API_TYPE, NULL);

  gst_query_parse_allocation(query, &caps, NULL);
  if (caps != NULL && gst_rga_caps_have_dmabuf(caps)) {
    /* The fds have to come from upstream, we have no way of exporting one.
     * Drop the system memory pool the parent class offers so that upstream
     * keeps allocating from its own DMABuf pool. */
#if GST_CHECK_VERSION(1, 16, 0)
    while (gst_query_get_n_allocation_pools(query) > 0)
      gst_query_remove_nth_allocation_pool(query, 0);
#endif
    GST_DEBUG_OBJECT(rgavideoconvert,
                     "sink caps carry " GST_CAPS_FEATURE_MEMORY_DMABUF
                     ", letting upstream allocate");
  }

  return TRUE;
}

static gboolean gst_rga_video_convert_decide_allocation(GstBaseTransform *trans,
                                                        GstQuery *query) {
  GstRgaVideoConvert *rgavideoconvert = gst_rga_video_convert(trans);
  GstCaps *caps = NULL;
  GstBufferPool *pool = NULL;
  guint size = 0, min = 0, max = 0;
  gboolean want_dmabuf;

  gst_query_parse_allocation(query, &caps, NULL);
  want_dmabuf = caps != NULL && gst_rga_caps_have_dmabuf(caps);

  if (gst_query_get_n_allocation_pools(query) > 0)
    gst_query_parse_nth_allocation_pool(query, 0, &pool, &size, &min, &max);

  if (want_dmabuf && pool == NULL) {
    /* RGA renders into memory somebody else owns: we can import a DMABuf but
     * not export one, so without a downstream pool the negotiated caps cannot
     * be honoured. */
    GST_ELEMENT_ERROR(rgavideoconvert, CORE, NEGOTIATION, (NULL),
                      ("downstream negotiated " GST_CAPS_FEATURE_MEMORY_DMABUF
                       " but does not offer a buffer pool"));
    return FALSE;
  }

  if (pool != NULL) {
    /* Ask for the video meta so the real strides of the pool buffers are
     * known, they are what gets handed to RGA. */
    if (gst_buffer_pool_has_option(pool, GST_BUFFER_POOL_OPTION_VIDEO_META)) {
      GstStructure *config = gst_buffer_pool_get_config(pool);

      gst_buffer_pool_config_add_option(config,
                                        GST_BUFFER_POOL_OPTION_VIDEO_META);
      if (!gst_buffer_pool_set_config(pool, config))
        GST_WARNING_OBJECT(rgavideoconvert,
                           "could not enable the video meta on the downstream "
                           "pool");
    }
    gst_object_unref(pool);
  }

  return GST_BASE_TRANSFORM_CLASS(gst_rga_video_convert_parent_class)
      ->decide_allocation(trans, query);
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

  /* These caps were negotiated for the rotation the property held while
   * transform_caps()/fixate_caps() ran. Latch it, so the frames RGA writes
   * always match the size of the buffers they go into. */
  GST_OBJECT_LOCK(rgavideoconvert);
  rgavideoconvert->active_rotation = rgavideoconvert->rotation;
  GST_OBJECT_UNLOCK(rgavideoconvert);

  GST_DEBUG_OBJECT(rgavideoconvert,
                   "%dx%d -> %dx%d, dmabuf in: %d, dmabuf out: %d",
                   GST_VIDEO_INFO_WIDTH(in_info),
                   GST_VIDEO_INFO_HEIGHT(in_info),
                   GST_VIDEO_INFO_WIDTH(out_info),
                   GST_VIDEO_INFO_HEIGHT(out_info),
                   gst_rga_caps_have_dmabuf(incaps),
                   gst_rga_caps_have_dmabuf(outcaps));
  return TRUE;
}

/* transform */

/* Zero-copy path: whenever one of the buffers is backed by a DMABuf its fd goes
 * straight to librga, so that buffer is never mapped for the CPU. When neither
 * side is a DMABuf, GstVideoFilter maps both frames and calls
 * transform_frame() below. */
static GstFlowReturn gst_rga_video_convert_transform(GstBaseTransform *trans,
                                                     GstBuffer *inbuf,
                                                     GstBuffer *outbuf) {
  GstRgaVideoConvert *rgavideoconvert = gst_rga_video_convert(trans);
  GstVideoFilter *filter = GST_VIDEO_FILTER(trans);
  GstRgaSurface src, dst;
  GstFlowReturn ret;

  if (G_UNLIKELY(!filter->negotiated)) {
    GST_ERROR_OBJECT(rgavideoconvert, "not negotiated");
    return GST_FLOW_NOT_NEGOTIATED;
  }

  if (gst_rga_buffer_get_dmabuf_fd(inbuf) < 0 &&
      gst_rga_buffer_get_dmabuf_fd(outbuf) < 0)
    return GST_BASE_TRANSFORM_CLASS(gst_rga_video_convert_parent_class)
        ->transform(trans, inbuf, outbuf);

  GST_LOG_OBJECT(rgavideoconvert, "transform (dmabuf)");

  if (!gst_rga_surface_open(rgavideoconvert, inbuf, &filter->in_info,
                            GST_MAP_READ, &src))
    return GST_FLOW_ERROR;

  if (!gst_rga_surface_open(rgavideoconvert, outbuf, &filter->out_info,
                            GST_MAP_WRITE, &dst)) {
    gst_rga_surface_close(&src);
    return GST_FLOW_ERROR;
  }

  ret = gst_rga_video_convert_run(rgavideoconvert, &src.buf, &dst.buf);

  gst_rga_surface_close(&dst);
  gst_rga_surface_close(&src);

  return ret;
}

static GstFlowReturn gst_rga_video_convert_transform_frame(
    GstVideoFilter *filter, GstVideoFrame *inframe, GstVideoFrame *outframe) {
  GstRgaVideoConvert *rgavideoconvert = gst_rga_video_convert(filter);

  GST_LOG_OBJECT(rgavideoconvert, "transform_frame (virtual address)");

  rga_buffer_t src = gst_rga_buffer_from_video_frame(inframe);
  rga_buffer_t dst = gst_rga_buffer_from_video_frame(outframe);

  return gst_rga_video_convert_run(rgavideoconvert, &src, &dst);
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
