/*
 * SPDX-License-Identifier: AGPL-3.0-or-later WITH Classpath-exception-2.0
 * Copyright (c) 2026 Saif Mukhtar
 *
 * OpenAVX - Open Audio-Video eXchange
 * src/avx_video_v4l2.c - Linux V4L2 Stateful M2M Hardware Decoder Backend
 */

#include "avx_video_codec.h"
#include "avx_internal.h"

#if defined(AVX_OS_LINUX)

#include <linux/videodev2.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <fcntl.h>
#include <poll.h>
#include <unistd.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define AVX_V4L2_MAX_DEVICES   64
#define AVX_V4L2_OUT_BUFS      4
#define AVX_V4L2_CAP_BUFS      6
#define AVX_V4L2_OUT_BUF_MIN   (2U * 1024U * 1024U)

typedef struct {
    void   *ptr;
    size_t  len;
    bool    in_use;
} avx_v4l2_buf_t;

typedef struct {
    int       fd;
    uint32_t  src_fourcc;
    uint32_t  v4l2_in_fmt;
    uint32_t  v4l2_cap_fmt;
    uint32_t  width;
    uint32_t  height;
    uint32_t  bytesperline[2];
    uint32_t  plane_sizes[2];
    uint32_t  cap_num_planes;

    avx_v4l2_buf_t out_bufs[AVX_V4L2_OUT_BUFS];
    uint32_t       out_count;

    struct {
        void   *planes[2];
        size_t  lens[2];
    } cap_bufs[AVX_V4L2_CAP_BUFS];
    uint32_t cap_count;

    bool     streaming_out;
    bool     streaming_cap;

    uint8_t *frame_buf;
    size_t   frame_buf_size;
} avx_v4l2_dec_ctx_t;

static avx_mutex_t s_v4l2_probe_lock = AVX_MUTEX_INITIALIZER;
static bool        s_v4l2_probed = false;
static char        s_v4l2_dev_h264[64];
static char        s_v4l2_dev_hevc[64];
static char        s_v4l2_dev_av1[64];

static int xioctl(int fd, unsigned long req, void *arg) {
    int rc;
    do {
        rc = ioctl(fd, req, arg);
    } while (rc == -1 && errno == EINTR);
    return rc;
}

static bool v4l2_enum_has_fmt(int fd, uint32_t buf_type, uint32_t target_pixfmt, bool require_compressed) {
    struct v4l2_fmtdesc fmtdesc;
    memset(&fmtdesc, 0, sizeof(fmtdesc));
    fmtdesc.type = buf_type;
    for (fmtdesc.index = 0; ; fmtdesc.index++) {
        if (xioctl(fd, VIDIOC_ENUM_FMT, &fmtdesc) != 0) break;
        if (fmtdesc.pixelformat == target_pixfmt) {
            if (require_compressed && !(fmtdesc.flags & V4L2_FMT_FLAG_COMPRESSED)) {
                return false;
            }
            return true;
        }
    }
    return false;
}

/* Probes /dev/video* devices via VIDIOC_QUERYCAP to discover V4L2 M2M hardware video decoders. */
static void v4l2_probe_devices_once(void) {
    avx_mutex_lock(&s_v4l2_probe_lock);
    if (s_v4l2_probed) {
        avx_mutex_unlock(&s_v4l2_probe_lock);
        return;
    }

    s_v4l2_dev_h264[0] = '\0';
    s_v4l2_dev_hevc[0] = '\0';
    s_v4l2_dev_av1[0]  = '\0';

    for (int i = 0; i < AVX_V4L2_MAX_DEVICES; ++i) {
        char path[64];
        snprintf(path, sizeof(path), "/dev/video%d", i);
        int fd = open(path, O_RDWR | O_NONBLOCK);
        if (fd < 0) continue;

        struct v4l2_capability cap;
        memset(&cap, 0, sizeof(cap));
        if (xioctl(fd, VIDIOC_QUERYCAP, &cap) != 0) {
            close(fd);
            continue;
        }

        uint32_t caps = (cap.capabilities & V4L2_CAP_DEVICE_CAPS) ? cap.device_caps : cap.capabilities;
        if (!(caps & V4L2_CAP_VIDEO_M2M_MPLANE) || !(caps & V4L2_CAP_STREAMING)) {
            close(fd);
            continue;
        }

        bool cap_has_yuv = v4l2_enum_has_fmt(fd, V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE, V4L2_PIX_FMT_NV12, false) ||
                           v4l2_enum_has_fmt(fd, V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE, V4L2_PIX_FMT_NV12M, false) ||
                           v4l2_enum_has_fmt(fd, V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE, V4L2_PIX_FMT_YUV420, false) ||
                           v4l2_enum_has_fmt(fd, V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE, V4L2_PIX_FMT_YUV420M, false);
        if (!cap_has_yuv) {
            close(fd);
            continue;
        }

#ifdef V4L2_PIX_FMT_H264
        if (s_v4l2_dev_h264[0] == '\0' &&
            v4l2_enum_has_fmt(fd, V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE, V4L2_PIX_FMT_H264, true)) {
            snprintf(s_v4l2_dev_h264, sizeof(s_v4l2_dev_h264), "%s", path);
        }
#endif
#ifdef V4L2_PIX_FMT_HEVC
        if (s_v4l2_dev_hevc[0] == '\0' &&
            v4l2_enum_has_fmt(fd, V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE, V4L2_PIX_FMT_HEVC, true)) {
            snprintf(s_v4l2_dev_hevc, sizeof(s_v4l2_dev_hevc), "%s", path);
        }
#endif
#ifdef V4L2_PIX_FMT_AV1
        if (s_v4l2_dev_av1[0] == '\0' &&
            v4l2_enum_has_fmt(fd, V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE, V4L2_PIX_FMT_AV1, true)) {
            snprintf(s_v4l2_dev_av1, sizeof(s_v4l2_dev_av1), "%s", path);
        }
#endif
        close(fd);
    }

    s_v4l2_probed = true;
    avx_mutex_unlock(&s_v4l2_probe_lock);
}

static const char *v4l2_select_device_and_fmt(uint32_t fourcc, uint32_t *out_pixfmt) {
    v4l2_probe_devices_once();

    if (fourcc == OAVX_FOURCC_VIDEO_TYPE_H264 || fourcc == 0x34363268) {
#ifdef V4L2_PIX_FMT_H264
        if (s_v4l2_dev_h264[0] != '\0') {
            if (out_pixfmt) *out_pixfmt = V4L2_PIX_FMT_H264;
            return s_v4l2_dev_h264;
        }
#endif
    } else if (fourcc == OAVX_FOURCC_VIDEO_TYPE_HEVC || fourcc == 0x43564568) {
#ifdef V4L2_PIX_FMT_HEVC
        if (s_v4l2_dev_hevc[0] != '\0') {
            if (out_pixfmt) *out_pixfmt = V4L2_PIX_FMT_HEVC;
            return s_v4l2_dev_hevc;
        }
#endif
    } else if (fourcc == OAVX_FOURCC_VIDEO_TYPE_AV01) {
#ifdef V4L2_PIX_FMT_AV1
        if (s_v4l2_dev_av1[0] != '\0') {
            if (out_pixfmt) *out_pixfmt = V4L2_PIX_FMT_AV1;
            return s_v4l2_dev_av1;
        }
#endif
    }
    return NULL;
}

static void v4l2_unmap_capture_bufs(avx_v4l2_dec_ctx_t *ctx) {
    for (uint32_t i = 0; i < ctx->cap_count; ++i) {
        for (uint32_t p = 0; p < 2; ++p) {
            if (ctx->cap_bufs[i].planes[p] && ctx->cap_bufs[i].planes[p] != MAP_FAILED) {
                munmap(ctx->cap_bufs[i].planes[p], ctx->cap_bufs[i].lens[p]);
            }
            ctx->cap_bufs[i].planes[p] = NULL;
            ctx->cap_bufs[i].lens[p] = 0;
        }
    }
    ctx->cap_count = 0;
}

/* Negotiates V4L2 capture queue format (NV12/YUV420M) and memory-maps driver buffers. */
static int v4l2_setup_capture_queue(avx_v4l2_dec_ctx_t *ctx) {
    struct v4l2_format cap_fmt;
    memset(&cap_fmt, 0, sizeof(cap_fmt));
    cap_fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;

    if (xioctl(ctx->fd, VIDIOC_G_FMT, &cap_fmt) != 0) {
        cap_fmt.fmt.pix_mp.width = ctx->width;
        cap_fmt.fmt.pix_mp.height = ctx->height;
    }

    uint32_t candidates[4] = {
        V4L2_PIX_FMT_NV12,
        V4L2_PIX_FMT_NV12M,
        V4L2_PIX_FMT_YUV420,
        V4L2_PIX_FMT_YUV420M
    };
    bool fmt_ok = false;
    for (int i = 0; i < 4; ++i) {
        struct v4l2_format try_fmt = cap_fmt;
        try_fmt.fmt.pix_mp.pixelformat = candidates[i];
        try_fmt.fmt.pix_mp.field = V4L2_FIELD_NONE;
        if (xioctl(ctx->fd, VIDIOC_S_FMT, &try_fmt) == 0 &&
            try_fmt.fmt.pix_mp.pixelformat == candidates[i]) {
            cap_fmt = try_fmt;
            fmt_ok = true;
            break;
        }
    }
    if (!fmt_ok && xioctl(ctx->fd, VIDIOC_G_FMT, &cap_fmt) != 0) {
        return -1;
    }

    ctx->v4l2_cap_fmt = cap_fmt.fmt.pix_mp.pixelformat;
    if (cap_fmt.fmt.pix_mp.width > 0)  ctx->width = cap_fmt.fmt.pix_mp.width;
    if (cap_fmt.fmt.pix_mp.height > 0) ctx->height = cap_fmt.fmt.pix_mp.height;
    ctx->cap_num_planes = cap_fmt.fmt.pix_mp.num_planes ? cap_fmt.fmt.pix_mp.num_planes : 1;
    if (ctx->cap_num_planes > 2) ctx->cap_num_planes = 2;
    for (uint32_t p = 0; p < ctx->cap_num_planes; ++p) {
        ctx->bytesperline[p] = cap_fmt.fmt.pix_mp.plane_fmt[p].bytesperline;
        ctx->plane_sizes[p]  = cap_fmt.fmt.pix_mp.plane_fmt[p].sizeimage;
    }

    struct v4l2_requestbuffers req;
    memset(&req, 0, sizeof(req));
    req.count = AVX_V4L2_CAP_BUFS;
    req.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
    req.memory = V4L2_MEMORY_MMAP;
    if (xioctl(ctx->fd, VIDIOC_REQBUFS, &req) != 0 || req.count == 0) {
        return -1;
    }
    if (req.count > AVX_V4L2_CAP_BUFS) req.count = AVX_V4L2_CAP_BUFS;
    ctx->cap_count = req.count;

    for (uint32_t i = 0; i < ctx->cap_count; ++i) {
        struct v4l2_plane planes[VIDEO_MAX_PLANES];
        struct v4l2_buffer buf;
        memset(planes, 0, sizeof(planes));
        memset(&buf, 0, sizeof(buf));
        buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
        buf.memory = V4L2_MEMORY_MMAP;
        buf.index = i;
        buf.length = ctx->cap_num_planes;
        buf.m.planes = planes;

        if (xioctl(ctx->fd, VIDIOC_QUERYBUF, &buf) != 0) return -1;

        for (uint32_t p = 0; p < ctx->cap_num_planes; ++p) {
            void *addr = mmap(NULL, planes[p].length, PROT_READ | PROT_WRITE, MAP_SHARED,
                              ctx->fd, planes[p].m.mem_offset);
            if (addr == MAP_FAILED) return -1;
            ctx->cap_bufs[i].planes[p] = addr;
            ctx->cap_bufs[i].lens[p] = planes[p].length;
        }
    }

    for (uint32_t i = 0; i < ctx->cap_count; ++i) {
        struct v4l2_plane planes[VIDEO_MAX_PLANES];
        struct v4l2_buffer buf;
        memset(planes, 0, sizeof(planes));
        memset(&buf, 0, sizeof(buf));
        buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
        buf.memory = V4L2_MEMORY_MMAP;
        buf.index = i;
        buf.length = ctx->cap_num_planes;
        buf.m.planes = planes;
        if (xioctl(ctx->fd, VIDIOC_QBUF, &buf) != 0) return -1;
    }

    uint32_t cap_type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
    if (xioctl(ctx->fd, VIDIOC_STREAMON, &cap_type) != 0) return -1;
    ctx->streaming_cap = true;
    return 0;
}

static bool v4l2_dec_supports_fourcc(uint32_t src_fourcc) {
    return v4l2_select_device_and_fmt(src_fourcc, NULL) != NULL;
}

static void v4l2_dec_destroy(void *backend_ctx) {
    avx_v4l2_dec_ctx_t *ctx = (avx_v4l2_dec_ctx_t *)backend_ctx;
    if (!ctx) return;

    if (ctx->fd >= 0) {
        if (ctx->streaming_out) {
            uint32_t t = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
            xioctl(ctx->fd, VIDIOC_STREAMOFF, &t);
            ctx->streaming_out = false;
        }
        if (ctx->streaming_cap) {
            uint32_t t = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
            xioctl(ctx->fd, VIDIOC_STREAMOFF, &t);
            ctx->streaming_cap = false;
        }
        for (uint32_t i = 0; i < ctx->out_count; ++i) {
            if (ctx->out_bufs[i].ptr && ctx->out_bufs[i].ptr != MAP_FAILED) {
                munmap(ctx->out_bufs[i].ptr, ctx->out_bufs[i].len);
            }
        }
        v4l2_unmap_capture_bufs(ctx);
        close(ctx->fd);
        ctx->fd = -1;
    }

    free(ctx->frame_buf);
    free(ctx);
}

static int v4l2_dec_create(const avx_video_dec_cfg_t *cfg, void **backend_ctx) {
    if (!cfg || !backend_ctx) return -1;

    uint32_t in_pixfmt = 0;
    const char *dev_path = v4l2_select_device_and_fmt(cfg->fourcc, &in_pixfmt);
    if (!dev_path) return -1;

    int fd = open(dev_path, O_RDWR | O_NONBLOCK);
    if (fd < 0) return -1;

    avx_v4l2_dec_ctx_t *ctx = (avx_v4l2_dec_ctx_t *)calloc(1, sizeof(*ctx));
    if (!ctx) {
        close(fd);
        return -1;
    }
    ctx->fd = fd;
    ctx->src_fourcc = cfg->fourcc;
    ctx->v4l2_in_fmt = in_pixfmt;
    ctx->width = cfg->width ? cfg->width : 1920;
    ctx->height = cfg->height ? cfg->height : 1080;

    struct v4l2_event_subscription sub;
    memset(&sub, 0, sizeof(sub));
    sub.type = V4L2_EVENT_SOURCE_CHANGE;
    xioctl(ctx->fd, VIDIOC_SUBSCRIBE_EVENT, &sub); /* Best-effort; not all drivers require */

    struct v4l2_format out_fmt;
    memset(&out_fmt, 0, sizeof(out_fmt));
    out_fmt.type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
    out_fmt.fmt.pix_mp.width = ctx->width;
    out_fmt.fmt.pix_mp.height = ctx->height;
    out_fmt.fmt.pix_mp.pixelformat = in_pixfmt;
    out_fmt.fmt.pix_mp.num_planes = 1;
    out_fmt.fmt.pix_mp.plane_fmt[0].sizeimage = AVX_V4L2_OUT_BUF_MIN;
    if (xioctl(ctx->fd, VIDIOC_S_FMT, &out_fmt) != 0) {
        v4l2_dec_destroy(ctx);
        return -1;
    }

    struct v4l2_requestbuffers req;
    memset(&req, 0, sizeof(req));
    req.count = AVX_V4L2_OUT_BUFS;
    req.type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
    req.memory = V4L2_MEMORY_MMAP;
    if (xioctl(ctx->fd, VIDIOC_REQBUFS, &req) != 0 || req.count == 0) {
        v4l2_dec_destroy(ctx);
        return -1;
    }
    if (req.count > AVX_V4L2_OUT_BUFS) req.count = AVX_V4L2_OUT_BUFS;
    ctx->out_count = req.count;

    for (uint32_t i = 0; i < ctx->out_count; ++i) {
        struct v4l2_plane planes[VIDEO_MAX_PLANES];
        struct v4l2_buffer buf;
        memset(planes, 0, sizeof(planes));
        memset(&buf, 0, sizeof(buf));
        buf.type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
        buf.memory = V4L2_MEMORY_MMAP;
        buf.index = i;
        buf.length = 1;
        buf.m.planes = planes;

        if (xioctl(ctx->fd, VIDIOC_QUERYBUF, &buf) != 0) {
            v4l2_dec_destroy(ctx);
            return -1;
        }
        void *addr = mmap(NULL, planes[0].length, PROT_READ | PROT_WRITE, MAP_SHARED,
                          ctx->fd, planes[0].m.mem_offset);
        if (addr == MAP_FAILED) {
            v4l2_dec_destroy(ctx);
            return -1;
        }
        ctx->out_bufs[i].ptr = addr;
        ctx->out_bufs[i].len = planes[0].length;
        ctx->out_bufs[i].in_use = false;
    }

    uint32_t out_type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
    if (xioctl(ctx->fd, VIDIOC_STREAMON, &out_type) != 0) {
        v4l2_dec_destroy(ctx);
        return -1;
    }
    ctx->streaming_out = true;

    if (v4l2_setup_capture_queue(ctx) != 0) {
        v4l2_dec_destroy(ctx);
        return -1;
    }

    *backend_ctx = ctx;
    return 0;
}

static void v4l2_reclaim_output_bufs(avx_v4l2_dec_ctx_t *ctx) {
    for (;;) {
        struct v4l2_plane planes[VIDEO_MAX_PLANES];
        struct v4l2_buffer buf;
        memset(planes, 0, sizeof(planes));
        memset(&buf, 0, sizeof(buf));
        buf.type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
        buf.memory = V4L2_MEMORY_MMAP;
        buf.length = 1;
        buf.m.planes = planes;
        if (xioctl(ctx->fd, VIDIOC_DQBUF, &buf) != 0) break;
        if (buf.index < ctx->out_count) {
            ctx->out_bufs[buf.index].in_use = false;
        }
    }
}

/* Handles V4L2_EVENT_SOURCE_CHANGE events, draining in-flight frames and reallocating capture buffers. */
static int v4l2_handle_source_change(avx_v4l2_dec_ctx_t *ctx) {
    struct v4l2_event ev;
    memset(&ev, 0, sizeof(ev));
    while (xioctl(ctx->fd, VIDIOC_DQEVENT, &ev) == 0) {
        if (ev.type == V4L2_EVENT_SOURCE_CHANGE &&
            (ev.u.src_change.changes & V4L2_EVENT_SRC_CH_RESOLUTION)) {
            if (ctx->streaming_cap) {
                uint32_t t = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
                xioctl(ctx->fd, VIDIOC_STREAMOFF, &t);
                ctx->streaming_cap = false;
            }
            v4l2_unmap_capture_bufs(ctx);

            struct v4l2_requestbuffers rel;
            memset(&rel, 0, sizeof(rel));
            rel.count = 0;
            rel.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
            rel.memory = V4L2_MEMORY_MMAP;
            xioctl(ctx->fd, VIDIOC_REQBUFS, &rel);

            if (v4l2_setup_capture_queue(ctx) != 0) return -1;
        }
    }
    return 0;
}

/* Queues compressed NAL packets to V4L2 output queue and drains decoded pictures from capture queue. */
static int v4l2_dec_decode(void *backend_ctx,
                           const uint8_t *pkt_data, size_t pkt_size,
                           oavx_video_frame_t *out_frame)
{
    avx_v4l2_dec_ctx_t *ctx = (avx_v4l2_dec_ctx_t *)backend_ctx;
    if (!ctx || ctx->fd < 0 || !pkt_data || pkt_size == 0 || !out_frame) {
        return AVX_VIDEO_CODEC_ERROR;
    }

    if (v4l2_handle_source_change(ctx) != 0) {
        return AVX_VIDEO_CODEC_ERROR;
    }

    v4l2_reclaim_output_bufs(ctx);

    int free_idx = -1;
    for (uint32_t i = 0; i < ctx->out_count; ++i) {
        if (!ctx->out_bufs[i].in_use) {
            free_idx = (int)i;
            break;
        }
    }
    if (free_idx < 0) {
        struct pollfd pfd = { .fd = ctx->fd, .events = POLLOUT, .revents = 0 };
        poll(&pfd, 1, 5);
        v4l2_reclaim_output_bufs(ctx);
        for (uint32_t i = 0; i < ctx->out_count; ++i) {
            if (!ctx->out_bufs[i].in_use) {
                free_idx = (int)i;
                break;
            }
        }
    }
    if (free_idx < 0 || pkt_size > ctx->out_bufs[free_idx].len) {
        return AVX_VIDEO_CODEC_ERROR;
    }

    memcpy(ctx->out_bufs[free_idx].ptr, pkt_data, pkt_size);

    struct v4l2_plane out_planes[VIDEO_MAX_PLANES];
    struct v4l2_buffer out_buf;
    memset(out_planes, 0, sizeof(out_planes));
    memset(&out_buf, 0, sizeof(out_buf));
    out_planes[0].bytesused = (uint32_t)pkt_size;
    out_buf.type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
    out_buf.memory = V4L2_MEMORY_MMAP;
    out_buf.index = (uint32_t)free_idx;
    out_buf.length = 1;
    out_buf.m.planes = out_planes;

    if (xioctl(ctx->fd, VIDIOC_QBUF, &out_buf) != 0) {
        return AVX_VIDEO_CODEC_ERROR;
    }
    ctx->out_bufs[free_idx].in_use = true;

    struct pollfd cap_pfd = { .fd = ctx->fd, .events = POLLIN | POLLPRI, .revents = 0 };
    poll(&cap_pfd, 1, 2);

    if (cap_pfd.revents & POLLPRI) {
        if (v4l2_handle_source_change(ctx) != 0) {
            return AVX_VIDEO_CODEC_ERROR;
        }
    }

    struct v4l2_plane cap_planes[VIDEO_MAX_PLANES];
    struct v4l2_buffer cap_buf;
    memset(cap_planes, 0, sizeof(cap_planes));
    memset(&cap_buf, 0, sizeof(cap_buf));
    cap_buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
    cap_buf.memory = V4L2_MEMORY_MMAP;
    cap_buf.length = ctx->cap_num_planes;
    cap_buf.m.planes = cap_planes;

    if (xioctl(ctx->fd, VIDIOC_DQBUF, &cap_buf) != 0) {
        if (errno == EAGAIN) {
            return AVX_VIDEO_CODEC_NEED_MORE_DATA;
        }
        if (errno == EPIPE) {
            if (v4l2_handle_source_change(ctx) == 0) {
                return AVX_VIDEO_CODEC_NEED_MORE_DATA;
            }
        }
        return AVX_VIDEO_CODEC_ERROR;
    }

    if ((cap_buf.flags & V4L2_BUF_FLAG_LAST) && cap_planes[0].bytesused == 0) {
        xioctl(ctx->fd, VIDIOC_QBUF, &cap_buf);
        if (v4l2_handle_source_change(ctx) != 0) {
            return AVX_VIDEO_CODEC_ERROR;
        }
        return AVX_VIDEO_CODEC_NEED_MORE_DATA;
    }

    uint32_t idx = cap_buf.index;
    uint32_t w = ctx->width;
    uint32_t h = ctx->height;
    uint64_t need_u64 = (uint64_t)w * (uint64_t)h * 2ULL;
    if (w == 0 || h == 0 || w > AVX_MAX_VIDEO_WIDTH || h > AVX_MAX_VIDEO_HEIGHT ||
        need_u64 > AVX_MAX_DECOMPRESSED_FRAME_SIZE || idx >= ctx->cap_count) {
        xioctl(ctx->fd, VIDIOC_QBUF, &cap_buf);
        return AVX_VIDEO_CODEC_ERROR;
    }

    uint32_t cw = (w + 1U) / 2U;
    uint32_t ch = (h + 1U) / 2U;
    bool is_nv12 = (ctx->v4l2_cap_fmt == V4L2_PIX_FMT_NV12 || ctx->v4l2_cap_fmt == V4L2_PIX_FMT_NV12M);
    size_t y_stride = ctx->bytesperline[0] ? ctx->bytesperline[0] : (size_t)w;
    size_t need = is_nv12
        ? ((size_t)w * (size_t)h + (size_t)w * (size_t)ch)
        : ((size_t)w * (size_t)h + 2ULL * (size_t)cw * (size_t)ch);
    if (ctx->frame_buf_size < need) {
        uint8_t *nb = (uint8_t *)realloc(ctx->frame_buf, need);
        if (!nb) {
            xioctl(ctx->fd, VIDIOC_QBUF, &cap_buf);
            return AVX_VIDEO_CODEC_ERROR;
        }
        ctx->frame_buf = nb;
        ctx->frame_buf_size = need;
    }

    const uint8_t *src_p0 = (const uint8_t *)ctx->cap_bufs[idx].planes[0];
    size_t src_p0_len = ctx->cap_bufs[idx].lens[0];

    if (ctx->v4l2_cap_fmt == V4L2_PIX_FMT_NV12 || ctx->v4l2_cap_fmt == V4L2_PIX_FMT_NV12M) {
        uint32_t ch = (h + 1) / 2;
        for (uint32_t y = 0; y < h; ++y) {
            if ((size_t)y * y_stride + w <= src_p0_len) {
                memcpy(ctx->frame_buf + (size_t)y * w, src_p0 + (size_t)y * y_stride, w);
            }
        }
        const uint8_t *src_uv = (ctx->cap_num_planes > 1 && ctx->cap_bufs[idx].planes[1])
                                ? (const uint8_t *)ctx->cap_bufs[idx].planes[1]
                                : (src_p0 + (size_t)h * y_stride);
        size_t uv_stride = (ctx->cap_num_planes > 1 && ctx->bytesperline[1]) ? ctx->bytesperline[1] : y_stride;
        size_t uv_max_len = (ctx->cap_num_planes > 1) ? ctx->cap_bufs[idx].lens[1] : src_p0_len;
        size_t uv_off = (ctx->cap_num_planes > 1) ? 0 : (size_t)h * y_stride;
        uint8_t *dst_uv = ctx->frame_buf + (size_t)w * h;
        for (uint32_t y = 0; y < ch; ++y) {
            if (uv_off + (size_t)y * uv_stride + w <= uv_max_len) {
                memcpy(dst_uv + (size_t)y * w, src_uv + (size_t)y * uv_stride, w);
            }
        }
        out_frame->fourcc = OAVX_FOURCC_VIDEO_TYPE_NV12;
        out_frame->width = w;
        out_frame->height = h;
        out_frame->planes[0] = ctx->frame_buf;
        out_frame->strides[0] = w;
        out_frame->planes[1] = dst_uv;
        out_frame->strides[1] = w;
        out_frame->planes[2] = NULL;
        out_frame->strides[2] = 0;
        out_frame->planes[3] = NULL;
        out_frame->strides[3] = 0;
    } else {
        /* Fallback I420 (single-plane contiguous Y + U + V) */
        uint32_t cw = (w + 1) / 2;
        uint32_t ch = (h + 1) / 2;
        for (uint32_t y = 0; y < h; ++y) {
            if ((size_t)y * y_stride + w <= src_p0_len) {
                memcpy(ctx->frame_buf + (size_t)y * w, src_p0 + (size_t)y * y_stride, w);
            }
        }
        const uint8_t *src_u = src_p0 + (size_t)h * y_stride;
        size_t c_stride = y_stride / 2;
        uint8_t *dst_u = ctx->frame_buf + (size_t)w * h;
        uint8_t *dst_v = dst_u + (size_t)cw * ch;
        for (uint32_t y = 0; y < ch; ++y) {
            if ((size_t)h * y_stride + (size_t)y * c_stride + cw <= src_p0_len) {
                memcpy(dst_u + (size_t)y * cw, src_u + (size_t)y * c_stride, cw);
            }
        }
        const uint8_t *src_v = src_u + (size_t)ch * c_stride;
        for (uint32_t y = 0; y < ch; ++y) {
            if ((size_t)h * y_stride + (size_t)ch * c_stride + (size_t)y * c_stride + cw <= src_p0_len) {
                memcpy(dst_v + (size_t)y * cw, src_v + (size_t)y * c_stride, cw);
            }
        }
        out_frame->fourcc = OAVX_FOURCC_VIDEO_TYPE_I420;
        out_frame->width = w;
        out_frame->height = h;
        out_frame->planes[0] = ctx->frame_buf;
        out_frame->strides[0] = w;
        out_frame->planes[1] = dst_u;
        out_frame->strides[1] = cw;
        out_frame->planes[2] = dst_v;
        out_frame->strides[2] = cw;
        out_frame->planes[3] = NULL;
        out_frame->strides[3] = 0;
    }

    xioctl(ctx->fd, VIDIOC_QBUF, &cap_buf);
    return AVX_VIDEO_CODEC_OK;
}

static void v4l2_dec_reset(void *backend_ctx) {
    avx_v4l2_dec_ctx_t *ctx = (avx_v4l2_dec_ctx_t *)backend_ctx;
    if (!ctx || ctx->fd < 0) return;

    if (ctx->streaming_out) {
        uint32_t t = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
        xioctl(ctx->fd, VIDIOC_STREAMOFF, &t);
    }
    if (ctx->streaming_cap) {
        uint32_t t = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
        xioctl(ctx->fd, VIDIOC_STREAMOFF, &t);
    }
    for (uint32_t i = 0; i < ctx->out_count; ++i) {
        ctx->out_bufs[i].in_use = false;
    }
    for (uint32_t i = 0; i < ctx->cap_count; ++i) {
        struct v4l2_plane planes[VIDEO_MAX_PLANES];
        struct v4l2_buffer buf;
        memset(planes, 0, sizeof(planes));
        memset(&buf, 0, sizeof(buf));
        buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
        buf.memory = V4L2_MEMORY_MMAP;
        buf.index = i;
        buf.length = ctx->cap_num_planes;
        buf.m.planes = planes;
        xioctl(ctx->fd, VIDIOC_QBUF, &buf);
    }
    if (ctx->streaming_out) {
        uint32_t t = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
        xioctl(ctx->fd, VIDIOC_STREAMON, &t);
    }
    if (ctx->streaming_cap) {
        uint32_t t = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
        xioctl(ctx->fd, VIDIOC_STREAMON, &t);
    }
}

static const avx_video_decoder_ops_t s_v4l2_decoder_ops = {
    .name = "v4l2-stateful",
    .priority = 75,
    .is_hardware = true,
    .supports_fourcc = v4l2_dec_supports_fourcc,
    .create = v4l2_dec_create,
    .decode = v4l2_dec_decode,
    .reset = v4l2_dec_reset,
    .destroy = v4l2_dec_destroy
};

const avx_video_decoder_ops_t *avx_video_v4l2_get_ops(void) {
    return &s_v4l2_decoder_ops;
}

#else

const avx_video_decoder_ops_t *avx_video_v4l2_get_ops(void) {
    return NULL;
}

#endif /* AVX_OS_LINUX */
