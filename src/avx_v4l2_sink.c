/*
 * SPDX-License-Identifier: AGPL-3.0-or-later WITH Classpath-exception-2.0
 * Copyright (c) 2026 Saif Mukhtar
 *
 * OpenAVX - Open Audio-Video eXchange
 * src/avx_v4l2_sink.c - Linux V4L2 Virtual Camera Loopback Sink
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "avx_v4l2_sink.h"
#include "avx_platform.h"
#include "avx_simd.h"
#include "avx_color.h"

#if defined(AVX_OS_LINUX)
    #include <fcntl.h>
    #include <unistd.h>
    #include <errno.h>
    #include <sys/ioctl.h>
    #include <linux/videodev2.h>
#endif

#if defined(AVX_OS_LINUX)

struct avx_v4l2_sink_t {
    int         fd;
    char        device_path[128];
    uint32_t    width;
    uint32_t    height;
    uint32_t    pixelformat;
    size_t      frame_bytes;
    uint8_t    *conv_buf;
    size_t      conv_buf_cap;
    avx_mutex_t lock;
};

/* Performs 32-bit word byte swap converting broadcast UYVY (U Y0 V Y1) to native V4L2 YUYV (Y0 U Y1 V). */
static void uyvy_to_yuyv_convert(const uint8_t *src, uint8_t *dst, size_t pixel_count)
{
    size_t word_count = pixel_count / 2;
    const uint32_t *src32 = (const uint32_t*)src;
    uint32_t *dst32 = (uint32_t*)dst;

    /* Process in 64-bit unrolled chunks */
    size_t i = 0;
    for (; i + 4 <= word_count; i += 4) {
        uint32_t v0 = src32[i + 0];
        uint32_t v1 = src32[i + 1];
        uint32_t v2 = src32[i + 2];
        uint32_t v3 = src32[i + 3];

        dst32[i + 0] = ((v0 & 0xFF00FF00) >> 8) | ((v0 & 0x00FF00FF) << 8);
        dst32[i + 1] = ((v1 & 0xFF00FF00) >> 8) | ((v1 & 0x00FF00FF) << 8);
        dst32[i + 2] = ((v2 & 0xFF00FF00) >> 8) | ((v2 & 0x00FF00FF) << 8);
        dst32[i + 3] = ((v3 & 0xFF00FF00) >> 8) | ((v3 & 0x00FF00FF) << 8);
    }
    for (; i < word_count; ++i) {
        uint32_t v = src32[i];
        dst32[i] = ((v & 0xFF00FF00) >> 8) | ((v & 0x00FF00FF) << 8);
    }
}

/* Convert I420 (planar Y, U, V) directly to packed YUYV (Y0 U Y1 V) */
static void i420_to_yuyv_convert(const uint8_t *y_src, size_t y_stride,
                                 const uint8_t *u_src, size_t u_stride,
                                 const uint8_t *v_src, size_t v_stride,
                                 uint8_t *dst, size_t dst_stride,
                                 int width, int height)
{
    if (!y_src || !u_src || !v_src || !dst || width <= 0 || height <= 0) return;
    if (y_stride == 0) y_stride = (size_t)width;
    if (u_stride == 0) u_stride = (size_t)(width + 1) / 2;
    if (v_stride == 0) v_stride = (size_t)(width + 1) / 2;
    if (dst_stride == 0) dst_stride = (size_t)width * 2;

    int half_h = (height + 1) / 2;
    int half_w = (width + 1) / 2;

    for (int y2 = 0; y2 < half_h; ++y2) {
        int y0 = y2 * 2;
        int y1 = y0 + 1;
        const uint8_t *y_row0 = y_src + (size_t)y0 * y_stride;
        const uint8_t *y_row1 = (y1 < height) ? (y_src + (size_t)y1 * y_stride) : y_row0;
        const uint8_t *u_row = u_src + (size_t)y2 * u_stride;
        const uint8_t *v_row = v_src + (size_t)y2 * v_stride;
        uint8_t *dst_row0 = dst + (size_t)y0 * dst_stride;
        uint8_t *dst_row1 = (y1 < height) ? (dst + (size_t)y1 * dst_stride) : NULL;

        for (int x2 = 0; x2 < half_w; ++x2) {
            uint8_t u = u_row[x2];
            uint8_t v = v_row[x2];
            uint8_t y00 = y_row0[2 * x2 + 0];
            uint8_t y01 = (2 * x2 + 1 < width) ? y_row0[2 * x2 + 1] : y00;

            dst_row0[4 * x2 + 0] = y00;
            dst_row0[4 * x2 + 1] = u;
            dst_row0[4 * x2 + 2] = y01;
            dst_row0[4 * x2 + 3] = v;

            if (dst_row1) {
                uint8_t y10 = y_row1[2 * x2 + 0];
                uint8_t y11 = (2 * x2 + 1 < width) ? y_row1[2 * x2 + 1] : y10;
                dst_row1[4 * x2 + 0] = y10;
                dst_row1[4 * x2 + 1] = u;
                dst_row1[4 * x2 + 2] = y11;
                dst_row1[4 * x2 + 3] = v;
            }
        }
    }
}

/* Convert NV12 (semi-planar Y, UV) directly to packed YUYV (Y0 U Y1 V) */
static void nv12_to_yuyv_convert(const uint8_t *y_src, size_t y_stride,
                                 const uint8_t *uv_src, size_t uv_stride,
                                 uint8_t *dst, size_t dst_stride,
                                 int width, int height)
{
    if (!y_src || !uv_src || !dst || width <= 0 || height <= 0) return;
    if (y_stride == 0) y_stride = (size_t)width;
    if (uv_stride == 0) uv_stride = (size_t)width;
    if (dst_stride == 0) dst_stride = (size_t)width * 2;

    int half_h = (height + 1) / 2;
    int half_w = (width + 1) / 2;

    for (int y2 = 0; y2 < half_h; ++y2) {
        int y0 = y2 * 2;
        int y1 = y0 + 1;
        const uint8_t *y_row0 = y_src + (size_t)y0 * y_stride;
        const uint8_t *y_row1 = (y1 < height) ? (y_src + (size_t)y1 * y_stride) : y_row0;
        const uint8_t *uv_row = uv_src + (size_t)y2 * uv_stride;
        uint8_t *dst_row0 = dst + (size_t)y0 * dst_stride;
        uint8_t *dst_row1 = (y1 < height) ? (dst + (size_t)y1 * dst_stride) : NULL;

        for (int x2 = 0; x2 < half_w; ++x2) {
            uint8_t u = uv_row[2 * x2 + 0];
            uint8_t v = uv_row[2 * x2 + 1];
            uint8_t y00 = y_row0[2 * x2 + 0];
            uint8_t y01 = (2 * x2 + 1 < width) ? y_row0[2 * x2 + 1] : y00;

            dst_row0[4 * x2 + 0] = y00;
            dst_row0[4 * x2 + 1] = u;
            dst_row0[4 * x2 + 2] = y01;
            dst_row0[4 * x2 + 3] = v;

            if (dst_row1) {
                uint8_t y10 = y_row1[2 * x2 + 0];
                uint8_t y11 = (2 * x2 + 1 < width) ? y_row1[2 * x2 + 1] : y10;
                dst_row1[4 * x2 + 0] = y10;
                dst_row1[4 * x2 + 1] = u;
                dst_row1[4 * x2 + 2] = y11;
                dst_row1[4 * x2 + 3] = v;
            }
        }
    }
}

/* Probes /dev/video* device nodes via VIDIOC_QUERYCAP to locate active v4l2loopback instances. */
bool avx_v4l2_sink_probe_loopback(char *out_path, size_t out_path_len)
{
    if (!out_path || out_path_len == 0) return false;

    char path[64];
    for (int i = 0; i < 64; ++i) {
        snprintf(path, sizeof(path), "/dev/video%d", i);
        int fd = open(path, O_RDWR | O_NONBLOCK);
        if (fd < 0) continue;

        struct v4l2_capability cap;
        memset(&cap, 0, sizeof(cap));
        int ret = ioctl(fd, VIDIOC_QUERYCAP, &cap);
        close(fd);

        if (ret < 0) continue;

        uint32_t caps = cap.capabilities;
        if (caps & V4L2_CAP_DEVICE_CAPS) {
            caps = cap.device_caps;
        }

        /* Check if device supports video output (writing frames into it) */
        if (caps & V4L2_CAP_VIDEO_OUTPUT) {
            const char *driver = (const char*)cap.driver;
            const char *card = (const char*)cap.card;

            if (strcasestr(driver, "loopback") ||
                strcasestr(card, "loopback") ||
                strcasestr(card, "OpenAVX") ||
                strcasestr(card, "Virtual") ||
                strcasestr(card, "Dummy")) {
                snprintf(out_path, out_path_len, "%s", path);
                return true;
            }
        }
    }

    return false;
}

/* Sets output resolution, pixel format, and field order on V4L2 loopback sink via VIDIOC_S_FMT. */
int avx_v4l2_sink_set_format(avx_v4l2_sink_t *sink, uint32_t width, uint32_t height, uint32_t pixelformat)
{
    if (!sink || sink->fd < 0) return -1;
    if (width == 0 || height == 0) return -1;

    if (pixelformat == 0) {
        pixelformat = V4L2_PIX_FMT_YUYV;
    }

    struct v4l2_format fmt;
    memset(&fmt, 0, sizeof(fmt));
    fmt.type = V4L2_BUF_TYPE_VIDEO_OUTPUT;
    fmt.fmt.pix.width = width;
    fmt.fmt.pix.height = height;
    fmt.fmt.pix.pixelformat = pixelformat;
    fmt.fmt.pix.field = V4L2_FIELD_NONE;
    fmt.fmt.pix.colorspace = V4L2_COLORSPACE_SRGB;

    /* Calculate frame size based on pixelformat */
    if (pixelformat == V4L2_PIX_FMT_YUYV || pixelformat == V4L2_PIX_FMT_UYVY) {
        fmt.fmt.pix.bytesperline = width * 2;
        fmt.fmt.pix.sizeimage = (size_t)width * height * 2;
    } else if (pixelformat == V4L2_PIX_FMT_BGR24 || pixelformat == V4L2_PIX_FMT_RGB24) {
        fmt.fmt.pix.bytesperline = width * 3;
        fmt.fmt.pix.sizeimage = (size_t)width * height * 3;
    } else {
        fmt.fmt.pix.bytesperline = width * 2;
        fmt.fmt.pix.sizeimage = (size_t)width * height * 2;
    }

    if (ioctl(sink->fd, VIDIOC_S_FMT, &fmt) < 0) {
        return -1;
    }

    sink->width = fmt.fmt.pix.width;
    sink->height = fmt.fmt.pix.height;
    sink->pixelformat = fmt.fmt.pix.pixelformat;
    sink->frame_bytes = fmt.fmt.pix.sizeimage;

    /* Ensure conversion buffer is appropriately sized */
    size_t need_cap = sink->frame_bytes > (width * height * 4) ? sink->frame_bytes : (width * height * 4);
    if (sink->conv_buf_cap < need_cap) {
        uint8_t *nb = (uint8_t*)realloc(sink->conv_buf, need_cap);
        if (!nb) return -1;
        sink->conv_buf = nb;
        sink->conv_buf_cap = need_cap;
    }

    return 0;
}

avx_v4l2_sink_t* avx_v4l2_sink_create(const avx_v4l2_sink_config_t *cfg)
{
    char dev_path[128];
    memset(dev_path, 0, sizeof(dev_path));

    if (cfg && cfg->device_path && cfg->device_path[0] != '\0') {
        snprintf(dev_path, sizeof(dev_path), "%s", cfg->device_path);
    } else {
        if (!avx_v4l2_sink_probe_loopback(dev_path, sizeof(dev_path))) {
            return NULL; /* No available loopback device discovered */
        }
    }

    int fd = open(dev_path, O_RDWR);
    if (fd < 0) {
        return NULL;
    }

    struct v4l2_capability cap;
    memset(&cap, 0, sizeof(cap));
    if (ioctl(fd, VIDIOC_QUERYCAP, &cap) < 0) {
        close(fd);
        return NULL;
    }

    uint32_t caps = cap.capabilities;
    if (caps & V4L2_CAP_DEVICE_CAPS) {
        caps = cap.device_caps;
    }
    if (!(caps & V4L2_CAP_VIDEO_OUTPUT)) {
        close(fd);
        return NULL; /* Not an output-capable V4L2 device */
    }

    avx_v4l2_sink_t *sink = (avx_v4l2_sink_t*)calloc(1, sizeof(avx_v4l2_sink_t));
    if (!sink) {
        close(fd);
        return NULL;
    }

    sink->fd = fd;
    snprintf(sink->device_path, sizeof(sink->device_path), "%s", dev_path);
    avx_mutex_init(&sink->lock);

    uint32_t w = (cfg && cfg->width > 0) ? cfg->width : 1920;
    uint32_t h = (cfg && cfg->height > 0) ? cfg->height : 1080;
    uint32_t pixfmt = (cfg && cfg->pixelformat > 0) ? cfg->pixelformat : V4L2_PIX_FMT_YUYV;

    if (avx_v4l2_sink_set_format(sink, w, h, pixfmt) != 0) {
        avx_v4l2_sink_destroy(sink);
        return NULL;
    }

    /* Set target output FPS via VIDIOC_S_PARM if provided */
    if (cfg && cfg->fps_num > 0) {
        struct v4l2_streamparm parm;
        memset(&parm, 0, sizeof(parm));
        parm.type = V4L2_BUF_TYPE_VIDEO_OUTPUT;
        parm.parm.output.timeperframe.numerator = (cfg->fps_den > 0) ? cfg->fps_den : 1;
        parm.parm.output.timeperframe.denominator = cfg->fps_num;
        (void)ioctl(sink->fd, VIDIOC_S_PARM, &parm);
    }

    return sink;
}

int avx_v4l2_sink_write_raw(avx_v4l2_sink_t *sink, const void *data, size_t size)
{
    if (!sink || sink->fd < 0 || !data || size == 0) return -1;

    avx_mutex_lock(&sink->lock);
    ssize_t written = write(sink->fd, data, size);
    avx_mutex_unlock(&sink->lock);

    if (written < 0) {
        return -1;
    }
    return 0;
}

/* Converts video frame (UYVY/RGBA/NV12/I420) to loopback device format and writes raw buffer. */
int avx_v4l2_sink_write_frame(avx_v4l2_sink_t *sink, const oavx_video_frame_t *frame)
{
    if (!sink || !frame || !frame->planes[0]) return -1;

    avx_mutex_lock(&sink->lock);

    /* Check if frame dimension changed and re-negotiate format if needed */
    if (frame->width != sink->width || frame->height != sink->height) {
        if (avx_v4l2_sink_set_format(sink, frame->width, frame->height, sink->pixelformat) != 0) {
            avx_mutex_unlock(&sink->lock);
            return -1;
        }
    }

    const void *send_data = frame->planes[0];
    size_t send_size = sink->frame_bytes;

    /* Handle format conversion to sink->pixelformat */
    if (sink->pixelformat == V4L2_PIX_FMT_YUYV) {
        if (frame->fourcc == OAVX_FOURCC_VIDEO_TYPE_UYVY) {
            /* Convert UYVY -> YUYV */
            size_t pixels = (size_t)frame->width * frame->height;
            size_t yuyv_bytes = pixels * 2;
            if (sink->conv_buf_cap >= yuyv_bytes) {
                uyvy_to_yuyv_convert(frame->planes[0], sink->conv_buf, pixels);
                send_data = sink->conv_buf;
                send_size = yuyv_bytes;
            }
        } else if (frame->fourcc == OAVX_FOURCC_VIDEO_TYPE_I420) {
            /* Convert Planar I420 -> Packed YUYV */
            size_t pixels = (size_t)frame->width * frame->height;
            size_t yuyv_bytes = pixels * 2;
            if (sink->conv_buf_cap >= yuyv_bytes) {
                i420_to_yuyv_convert(
                    frame->planes[0], frame->strides[0],
                    frame->planes[1], frame->strides[1],
                    frame->planes[2], frame->strides[2],
                    sink->conv_buf, (size_t)frame->width * 2,
                    (int)frame->width, (int)frame->height
                );
                send_data = sink->conv_buf;
                send_size = yuyv_bytes;
            }
        } else if (frame->fourcc == OAVX_FOURCC_VIDEO_TYPE_YV12) {
            /* Convert Planar YV12 (V before U) -> Packed YUYV */
            size_t pixels = (size_t)frame->width * frame->height;
            size_t yuyv_bytes = pixels * 2;
            if (sink->conv_buf_cap >= yuyv_bytes) {
                i420_to_yuyv_convert(
                    frame->planes[0], frame->strides[0],
                    frame->planes[2], frame->strides[2],
                    frame->planes[1], frame->strides[1],
                    sink->conv_buf, (size_t)frame->width * 2,
                    (int)frame->width, (int)frame->height
                );
                send_data = sink->conv_buf;
                send_size = yuyv_bytes;
            }
        } else if (frame->fourcc == OAVX_FOURCC_VIDEO_TYPE_NV12) {
            /* Convert Semi-planar NV12 -> Packed YUYV */
            size_t pixels = (size_t)frame->width * frame->height;
            size_t yuyv_bytes = pixels * 2;
            if (sink->conv_buf_cap >= yuyv_bytes) {
                nv12_to_yuyv_convert(
                    frame->planes[0], frame->strides[0],
                    frame->planes[1], frame->strides[1],
                    sink->conv_buf, (size_t)frame->width * 2,
                    (int)frame->width, (int)frame->height
                );
                send_data = sink->conv_buf;
                send_size = yuyv_bytes;
            }
        } else if (frame->fourcc == OAVX_FOURCC_VIDEO_TYPE_RGBA || frame->fourcc == OAVX_FOURCC_VIDEO_TYPE_RGBX) {
            /* Convert RGBA -> UYVY, then UYVY -> YUYV */
            size_t pixels = (size_t)frame->width * frame->height;
            size_t yuyv_bytes = pixels * 2;
            if (sink->conv_buf_cap >= yuyv_bytes * 2) {
                uint8_t *tmp_uyvy = sink->conv_buf + yuyv_bytes;
                avx_color_rgba_to_uyvy(frame->planes[0], tmp_uyvy, (int)frame->width, (int)frame->height);
                uyvy_to_yuyv_convert(tmp_uyvy, sink->conv_buf, pixels);
                send_data = sink->conv_buf;
                send_size = yuyv_bytes;
            }
        } else if (frame->fourcc == OAVX_FOURCC_VIDEO_TYPE_BGRA || frame->fourcc == OAVX_FOURCC_VIDEO_TYPE_BGRX) {
            /* Convert BGRA -> UYVY, then UYVY -> YUYV */
            size_t pixels = (size_t)frame->width * frame->height;
            size_t yuyv_bytes = pixels * 2;
            if (sink->conv_buf_cap >= yuyv_bytes * 2) {
                uint8_t *tmp_uyvy = sink->conv_buf + yuyv_bytes;
                avx_color_bgra_to_uyvy(frame->planes[0], tmp_uyvy, (int)frame->width, (int)frame->height);
                uyvy_to_yuyv_convert(tmp_uyvy, sink->conv_buf, pixels);
                send_data = sink->conv_buf;
                send_size = yuyv_bytes;
            }
        }
    } else if (sink->pixelformat == V4L2_PIX_FMT_UYVY) {
        if (frame->fourcc == OAVX_FOURCC_VIDEO_TYPE_UYVY) {
            /* Already UYVY: zero-copy passthrough */
            send_data = frame->planes[0];
            send_size = (size_t)frame->width * frame->height * 2;
        } else if (frame->fourcc == OAVX_FOURCC_VIDEO_TYPE_I420) {
            /* Convert Planar I420 -> Packed UYVY */
            size_t uyvy_bytes = (size_t)frame->width * frame->height * 2;
            if (sink->conv_buf_cap >= uyvy_bytes) {
                avx_color_i420_to_uyvy(
                    frame->planes[0], frame->strides[0],
                    frame->planes[1], frame->strides[1],
                    frame->planes[2], frame->strides[2],
                    sink->conv_buf, (size_t)frame->width * 2,
                    (int)frame->width, (int)frame->height
                );
                send_data = sink->conv_buf;
                send_size = uyvy_bytes;
            }
        } else if (frame->fourcc == OAVX_FOURCC_VIDEO_TYPE_YV12) {
            /* Convert Planar YV12 -> Packed UYVY */
            size_t uyvy_bytes = (size_t)frame->width * frame->height * 2;
            if (sink->conv_buf_cap >= uyvy_bytes) {
                avx_color_i420_to_uyvy(
                    frame->planes[0], frame->strides[0],
                    frame->planes[2], frame->strides[2],
                    frame->planes[1], frame->strides[1],
                    sink->conv_buf, (size_t)frame->width * 2,
                    (int)frame->width, (int)frame->height
                );
                send_data = sink->conv_buf;
                send_size = uyvy_bytes;
            }
        } else if (frame->fourcc == OAVX_FOURCC_VIDEO_TYPE_NV12) {
            /* Convert Semi-planar NV12 -> Packed UYVY */
            size_t uyvy_bytes = (size_t)frame->width * frame->height * 2;
            if (sink->conv_buf_cap >= uyvy_bytes) {
                avx_color_nv12_to_uyvy(
                    frame->planes[0], frame->strides[0],
                    frame->planes[1], frame->strides[1],
                    sink->conv_buf, (size_t)frame->width * 2,
                    (int)frame->width, (int)frame->height
                );
                send_data = sink->conv_buf;
                send_size = uyvy_bytes;
            }
        } else if (frame->fourcc == OAVX_FOURCC_VIDEO_TYPE_RGBA || frame->fourcc == OAVX_FOURCC_VIDEO_TYPE_RGBX) {
            size_t uyvy_bytes = (size_t)frame->width * frame->height * 2;
            if (sink->conv_buf_cap >= uyvy_bytes) {
                avx_color_rgba_to_uyvy(frame->planes[0], sink->conv_buf, (int)frame->width, (int)frame->height);
                send_data = sink->conv_buf;
                send_size = uyvy_bytes;
            }
        } else if (frame->fourcc == OAVX_FOURCC_VIDEO_TYPE_BGRA || frame->fourcc == OAVX_FOURCC_VIDEO_TYPE_BGRX) {
            size_t uyvy_bytes = (size_t)frame->width * frame->height * 2;
            if (sink->conv_buf_cap >= uyvy_bytes) {
                avx_color_bgra_to_uyvy(frame->planes[0], sink->conv_buf, (int)frame->width, (int)frame->height);
                send_data = sink->conv_buf;
                send_size = uyvy_bytes;
            }
        }
    }

    ssize_t written = write(sink->fd, send_data, send_size);
    avx_mutex_unlock(&sink->lock);

    return (written >= 0) ? 0 : -1;
}

const char* avx_v4l2_sink_get_device_path(const avx_v4l2_sink_t *sink)
{
    return sink ? sink->device_path : "";
}

void avx_v4l2_sink_destroy(avx_v4l2_sink_t *sink)
{
    if (!sink) return;
    avx_mutex_lock(&sink->lock);
    if (sink->fd >= 0) {
        close(sink->fd);
        sink->fd = -1;
    }
    if (sink->conv_buf) {
        free(sink->conv_buf);
        sink->conv_buf = NULL;
        sink->conv_buf_cap = 0;
    }
    avx_mutex_unlock(&sink->lock);
    avx_mutex_destroy(&sink->lock);
    free(sink);
}

#else /* !AVX_OS_LINUX */

/* Portable non-Linux fallback stubs */
/* Probes /dev/video* device nodes via VIDIOC_QUERYCAP to locate active v4l2loopback instances. */
bool avx_v4l2_sink_probe_loopback(char *out_path, size_t out_path_len)
{
    (void)out_path; (void)out_path_len;
    return false;
}

avx_v4l2_sink_t* avx_v4l2_sink_create(const avx_v4l2_sink_config_t *cfg)
{
    (void)cfg;
    return NULL;
}

/* Sets output resolution, pixel format, and field order on V4L2 loopback sink via VIDIOC_S_FMT. */
int avx_v4l2_sink_set_format(avx_v4l2_sink_t *sink, uint32_t width, uint32_t height, uint32_t pixelformat)
{
    (void)sink; (void)width; (void)height; (void)pixelformat;
    return -1;
}

int avx_v4l2_sink_write_raw(avx_v4l2_sink_t *sink, const void *data, size_t size)
{
    (void)sink; (void)data; (void)size;
    return -1;
}

/* Converts video frame (UYVY/RGBA/NV12/I420) to loopback device format and writes raw buffer. */
int avx_v4l2_sink_write_frame(avx_v4l2_sink_t *sink, const oavx_video_frame_t *frame)
{
    (void)sink; (void)frame;
    return -1;
}

const char* avx_v4l2_sink_get_device_path(const avx_v4l2_sink_t *sink)
{
    (void)sink;
    return "";
}

void avx_v4l2_sink_destroy(avx_v4l2_sink_t *sink)
{
    (void)sink;
}

#endif /* AVX_OS_LINUX */
