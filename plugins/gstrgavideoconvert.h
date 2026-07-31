/* GStreamer
 * Copyright (C) 2021 FIXME <fixme@example.com>
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
 * Free Software Foundation, Inc., 51 Franklin St, Fifth Floor,
 * Boston, MA 02110-1301, USA.
 */

#ifndef PLUGINS_GSTRGAVIDEOCONVERT_H_
#define PLUGINS_GSTRGAVIDEOCONVERT_H_

#include <gst/video/gstvideofilter.h>
#include <gst/video/video.h>

G_BEGIN_DECLS

#define GST_TYPE_RGA_VIDEO_CONVERT (gst_rga_video_convert_get_type())
#define gst_rga_video_convert(obj)                               \
  (G_TYPE_CHECK_INSTANCE_CAST((obj), GST_TYPE_RGA_VIDEO_CONVERT, \
                              GstRgaVideoConvert))
#define gst_rga_video_convert_CLASS(klass)                      \
  (G_TYPE_CHECK_CLASS_CAST((klass), GST_TYPE_RGA_VIDEO_CONVERT, \
                           GstRgaVideoConvertClass))
#define GST_IS_RGA_VIDEO_CONVERT(obj) \
  (G_TYPE_CHECK_INSTANCE_TYPE((obj), GST_TYPE_RGA_VIDEO_CONVERT))
#define GST_IS_RGA_VIDEO_CONVERT_CLASS(obj) \
  (G_TYPE_CHECK_CLASS_TYPE((klass), GST_TYPE_RGA_VIDEO_CONVERT))

typedef struct _GstRgaVideoConvert GstRgaVideoConvert;
typedef struct _GstRgaVideoConvertClass GstRgaVideoConvertClass;

struct _GstRgaVideoConvert {
  GstVideoFilter base_rgavideoconvert;
  /* All of these are guarded by the GstObject lock. */
  guint32 core_mask;
  guint32 flip;
  /* What the property says. Used while negotiating, since a 90/270 rotation
   * transposes the output caps. */
  guint32 rotation;
  /* The rotation the currently negotiated caps were built for. This is the one
   * handed to RGA, so that a property change mid-stream cannot rotate a frame
   * into a buffer that was sized for the previous angle. */
  guint32 active_rotation;

  /* Scratch buffer RGA converts into on the DMABuf path, instead of writing
   * straight into the externally-pooled outbuf. Some downstream HW elements
   * drop their reference to a buffer as soon as it is submitted to hardware,
   * before their own asynchronous read of it has actually finished - with a
   * single-buffer pool that means the very next outbuf we are handed can be
   * the same physical memory a downstream encoder may still be reading. RGA
   * (which may itself complete asynchronously relative to improcess()
   * returning) is kept off that shared memory entirely; only a plain,
   * synchronous CPU copy (gst_video_frame_copy) ever touches outbuf, right
   * before we return. Only used from the streaming thread, so unlike
   * core_mask/flip/rotation this needs no lock. */
  GstBuffer *scratch_buffer;
  GstVideoInfo scratch_info;

  /* Debug: how recently each currently-cycling DMABuf fd was last imported,
   * to check for a suspiciously tight reuse cadence (the same class of race
   * already found on the encoder's output-buffer side). Indexed by
   * fd % G_N_ELEMENTS(...); good enough for the handful of buffers a v4l2
   * capture pool actually cycles through. -1 in fd_last_seen_fd means unset. */
  gint64 fd_last_seen_us[8];
  gint fd_last_seen_fd[8];
};

struct _GstRgaVideoConvertClass {
  GstVideoFilterClass base_rgavideoconvert_class;
};

GType gst_rga_video_convert_get_type(void);

G_END_DECLS

#endif  // PLUGINS_GSTRGAVIDEOCONVERT_H_
