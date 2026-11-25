/* v4l2grab: capture N frames from a V4L2 device and print per-plane
 * statistics (min/avg/max) to characterise green-screen / dim issues.
 *
 * Usage: v4l2grab dev [frames] [out_raw_prefix] [exposure] [gain]
 *
 * When exposure / gain are given they are pushed with VIDIOC_S_CTRL before
 * streaming starts (0 means "leave alone"). Useful to sweep the sensor gain
 * mapping without rebuilding the HAL.
 *
 * For 10-bit Bayer formats it also prints the average of each of the four
 * Bayer sub-lattice positions (p00/p01/p10/p11), which shows channel balance
 * (a green cast is a p01/p10 vs p00/p11 imbalance) without demosaicing.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <linux/videodev2.h>

static int xioctl(int fd, unsigned req, void* arg) {
    int r;
    do { r = ioctl(fd, req, arg); } while (r < 0 && errno == EINTR);
    return r;
}

static void plane_stats(const unsigned char* p, size_t n, const char* tag, int idx) {
    unsigned long long sum = 0; int mn = 255, mx = 0; size_t i;
    for (i = 0; i < n; i++) {
        sum += p[i];
        if (p[i] < mn) mn = p[i];
        if (p[i] > mx) mx = p[i];
    }
    printf("frame%d %s: n=%zu min=%d avg=%.1f max=%d\n", idx, tag, n, mn,
           n ? (double)sum / n : 0.0, mx);
}

/* Bayer sub-lattice statistics for 10-bit raw stored as 16-bit LE samples. */
static void bayer_stats(const unsigned char* p, unsigned w, unsigned h, int idx) {
    unsigned long long s[2][2] = {{0, 0}, {0, 0}};
    unsigned long long c[2][2] = {{0, 0}, {0, 0}};
    unsigned mx[2][2] = {{0, 0}, {0, 0}};
    for (unsigned y = 0; y + 1 < h; y += 2) {
        const unsigned char* row = p + (size_t)y * w * 2;
        for (unsigned x = 0; x + 1 < w; x += 2) {
            for (unsigned dy = 0; dy < 2; dy++) {
                for (unsigned dx = 0; dx < 2; dx++) {
                    const unsigned char* q = row + (size_t)dy * w * 2 + (size_t)(x + dx) * 2;
                    unsigned v = q[0] | (q[1] << 8);
                    s[dy][dx] += v; c[dy][dx]++;
                    if (v > mx[dy][dx]) mx[dy][dx] = v;
                }
            }
        }
    }
    printf("frame%d BAYER10: p00=%.1f(max %u) p01=%.1f(max %u) p10=%.1f(max %u) p11=%.1f(max %u)\n",
           idx,
           c[0][0] ? (double)s[0][0] / c[0][0] : 0.0, mx[0][0],
           c[0][1] ? (double)s[0][1] / c[0][1] : 0.0, mx[0][1],
           c[1][0] ? (double)s[1][0] / c[1][0] : 0.0, mx[1][0],
           c[1][1] ? (double)s[1][1] / c[1][1] : 0.0, mx[1][1]);
}

static void set_ctrl(int fd, unsigned id, int value, const char* name) {
    struct v4l2_control c;
    memset(&c, 0, sizeof(c));
    c.id = id; c.value = value;
    if (xioctl(fd, VIDIOC_S_CTRL, &c) < 0) {
        printf("set %s=%d failed: %s\n", name, value, strerror(errno));
    } else {
        struct v4l2_control g;
        memset(&g, 0, sizeof(g));
        g.id = id;
        if (xioctl(fd, VIDIOC_G_CTRL, &g) == 0)
            printf("set %s=%d -> readback %d\n", name, value, g.value);
        else
            printf("set %s=%d ok\n", name, value);
    }
}

int main(int argc, char** argv) {
    if (argc < 2) {
        printf("usage: %s dev [frames] [outprefix] [exposure] [gain]\n", argv[0]);
        return 1;
    }
    const char* dev = argv[1];
    int nframes = argc > 2 ? atoi(argv[2]) : 5;
    const char* outp = (argc > 3 && strcmp(argv[3], "-") != 0) ? argv[3] : NULL;
    int exposure = argc > 4 ? atoi(argv[4]) : 0;
    int gain = argc > 5 ? atoi(argv[5]) : 0;

    int fd = open(dev, O_RDWR);
    if (fd < 0) { printf("open %s: %s\n", dev, strerror(errno)); return 1; }

    struct v4l2_capability cap;
    if (xioctl(fd, VIDIOC_QUERYCAP, &cap) < 0) { printf("querycap: %s\n", strerror(errno)); return 1; }
    printf("driver=%s card=%s bus=%s caps=%#x\n", cap.driver, cap.card, cap.bus_info, cap.capabilities);

    struct v4l2_format fmt;
    memset(&fmt, 0, sizeof(fmt));
    fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    if (xioctl(fd, VIDIOC_G_FMT, &fmt) < 0) { printf("gfmt: %s\n", strerror(errno)); return 1; }
    printf("fmt: %ux%u fourcc=%c%c%c%c bytesperline=%u sizeimage=%u field=%u\n",
           fmt.fmt.pix.width, fmt.fmt.pix.height,
           fmt.fmt.pix.pixelformat & 0xff, (fmt.fmt.pix.pixelformat >> 8) & 0xff,
           (fmt.fmt.pix.pixelformat >> 16) & 0xff, (fmt.fmt.pix.pixelformat >> 24) & 0xff,
           fmt.fmt.pix.bytesperline, fmt.fmt.pix.sizeimage, fmt.fmt.pix.field);

    if (exposure > 0) set_ctrl(fd, V4L2_CID_EXPOSURE, exposure, "exposure");
    if (gain > 0) set_ctrl(fd, V4L2_CID_GAIN, gain, "gain");

    unsigned bufs = 4;
    struct v4l2_requestbuffers rb;
    memset(&rb, 0, sizeof(rb));
    rb.count = bufs; rb.type = V4L2_BUF_TYPE_VIDEO_CAPTURE; rb.memory = V4L2_MEMORY_MMAP;
    if (xioctl(fd, VIDIOC_REQBUFS, &rb) < 0) { printf("reqbufs: %s\n", strerror(errno)); return 1; }
    printf("buffers=%u\n", rb.count);

    struct { void* start; size_t len; } bu[8];
    for (unsigned i = 0; i < rb.count; i++) {
        struct v4l2_buffer b;
        memset(&b, 0, sizeof(b));
        b.type = V4L2_BUF_TYPE_VIDEO_CAPTURE; b.memory = V4L2_MEMORY_MMAP; b.index = i;
        if (xioctl(fd, VIDIOC_QUERYBUF, &b) < 0) { printf("querybuf: %s\n", strerror(errno)); return 1; }
        bu[i].len = b.length;
        bu[i].start = mmap(NULL, b.length, PROT_READ | PROT_WRITE, MAP_SHARED, fd, b.m.offset);
        if (bu[i].start == MAP_FAILED) { printf("mmap: %s\n", strerror(errno)); return 1; }
        memset(bu[i].start, 0, b.length);
    }
    enum v4l2_buf_type t = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    for (unsigned i = 0; i < rb.count; i++) {
        struct v4l2_buffer b;
        memset(&b, 0, sizeof(b));
        b.type = t; b.memory = V4L2_MEMORY_MMAP; b.index = i;
        if (xioctl(fd, VIDIOC_QBUF, &b) < 0) { printf("qbuf: %s\n", strerror(errno)); return 1; }
    }
    if (xioctl(fd, VIDIOC_STREAMON, &t) < 0) { printf("streamon: %s\n", strerror(errno)); return 1; }

    unsigned fourcc = fmt.fmt.pix.pixelformat;
    unsigned w = fmt.fmt.pix.width, h = fmt.fmt.pix.height;
    int is_bayer10 = (fourcc == V4L2_PIX_FMT_SBGGR10 || fourcc == V4L2_PIX_FMT_SRGGB10 ||
                      fourcc == V4L2_PIX_FMT_SGRBG10 || fourcc == V4L2_PIX_FMT_SGBRG10);
    for (int n = 0; n < nframes; n++) {
        struct v4l2_buffer b;
        memset(&b, 0, sizeof(b));
        b.type = t; b.memory = V4L2_MEMORY_MMAP;
        if (xioctl(fd, VIDIOC_DQBUF, &b) < 0) { printf("dqbuf frame%d: %s\n", n, strerror(errno)); break; }
        printf("frame%d seq=%u bytesused=%u\n", n, b.sequence, b.bytesused);
        plane_stats(bu[b.index].start, b.bytesused, "ALL", n);
        if (is_bayer10)
            bayer_stats(bu[b.index].start, w, h, n);
        if (fourcc == V4L2_PIX_FMT_YUYV || fourcc == V4L2_PIX_FMT_UYVY) {
            /* planes are interleaved; synthesize Y and chroma views */
            const unsigned char* p = bu[b.index].start;
            plane_stats(p, b.bytesused / 2, fourcc == V4L2_PIX_FMT_YUYV ? "Y(YUYV)" : "UV/2(YUYV)", n);
        }
        if (outp && n < 3) {
            char fn[256];
            snprintf(fn, sizeof(fn), "%s_%02d.raw", outp, n);
            FILE* f = fopen(fn, "wb");
            if (f) { fwrite(bu[b.index].start, 1, b.bytesused, f); fclose(f); printf("saved %s\n", fn); }
        }
        if (xioctl(fd, VIDIOC_QBUF, &b) < 0) break;
    }

    xioctl(fd, VIDIOC_STREAMOFF, &t);
    close(fd);
    return 0;
}
