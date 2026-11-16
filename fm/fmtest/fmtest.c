/*
 * fmtest - FM chip chain diagnostic tool for Xiaomi Mi Pad 1 (mocha)
 *
 * Goal: bypass the Android framework (FMRadio app / libfmjni / AudioManager headset
 *       detection) and issue standard V4L2 radio ioctls straight to /dev/radio0 to verify
 *       the chain  kernel v4l2_fm_driver  ->  brcm-uim-sysfs  ->  BCM4354 FM chip
 *       actually works.
 *
 * Usage (root):
 *   fmtest             # full diagnosis: QUERYCAP / G_TUNER / tune / RSSI sweep
 *   fmtest 97400       # tune to the given frequency (kHz) and read RSSI
 *
 * Note: does not touch the audio path (I2S -> codec); audio needs headphones on the device.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <termios.h>
#include <sys/ioctl.h>
#include <sys/select.h>
#include <linux/videodev2.h>

#define FM_DEV "/dev/radio0"

/* V4L2 radio frequencies are in units of 62.5 Hz, i.e. 1 kHz = 16 */
#define KHZ_TO_V4L2(khz) ((khz) * 16)

/* HCI Reset command (OGF=0x03 Host Control, OCF=0x0003): 01 03 0C 00 */
static const unsigned char hci_reset_cmd[] = { 0x01, 0x03, 0x0C, 0x00 };

/*
 * uart_probe - send one HCI Reset to the given tty and see whether the BCM chip answers
 *              Command Complete (04 0E 04 01 03 0C 00)。
 *
 * Why this exists: brcm-uim-sysfs takes the port name from UimUartPort in
 * bt_vendor.conf. If that value does not match the UART the BT chip is actually wired
 * to, the chip never responds, uim blocks in read_hci_event and the kernel sees it as
 *     line disc installation timed out
 *     Failed to get shared ldisc write func pointer
 * Scanning every ttyTHS is the quickest way to settle it.
 */
static int uart_probe_once(const char *dev, int baud, int hwflow)
{
    int fd, i, n;
    struct termios tio;
    fd_set rfds;
    struct timeval tv;
    unsigned char buf[64];

    fd = open(dev, O_RDWR | O_NOCTTY | O_NONBLOCK);
    if (fd < 0) {
        printf("  %-16s open: %s\n", dev, strerror(errno));
        return -1;
    }

    if (tcgetattr(fd, &tio) < 0) {
        printf("  %-16s tcgetattr: %s\n", dev, strerror(errno));
        close(fd);
        return -1;
    }
    cfmakeraw(&tio);
    tio.c_cflag |= (CLOCAL | CREAD);
    if (hwflow)
        tio.c_cflag |= CRTSCTS;
    else
        tio.c_cflag &= ~CRTSCTS;
    tio.c_cc[VMIN]  = 0;
    tio.c_cc[VTIME] = 0;
    cfsetispeed(&tio, baud);
    cfsetospeed(&tio, baud);
    tcsetattr(fd, TCSANOW, &tio);
    tcflush(fd, TCIOFLUSH);

    n = write(fd, hci_reset_cmd, sizeof(hci_reset_cmd));
    if (n != (int)sizeof(hci_reset_cmd)) {
        printf("  %-16s write=%d (%s)\n", dev, n, strerror(errno));
        close(fd);
        return -1;
    }

    /* wait up to 600ms for a reply */
    i = 0;
    while (i < 6) {
        FD_ZERO(&rfds);
        FD_SET(fd, &rfds);
        tv.tv_sec  = 0;
        tv.tv_usec = 100 * 1000;
        n = select(fd + 1, &rfds, NULL, NULL, &tv);
        if (n > 0) {
            int r = read(fd, buf, sizeof(buf));
            if (r > 0) {
                int k;
                printf("  %-16s [flow=%d] got %d bytes:", dev, hwflow, r);
                for (k = 0; k < r; k++)
                    printf(" %02x", buf[k]);
                printf("%s\n", (buf[0] == 0x04) ? "   <== HCI event, this is it!" : "");
                close(fd);
                return (buf[0] == 0x04) ? 1 : 0;
            }
        } else if (n == 0) {
            i++;
        } else {
            if (errno == EINTR) continue;
            break;
        }
    }

    printf("  %-16s [flow=%d] no response\n", dev, hwflow);
    close(fd);
    return 0;
}

static int uart_probe(const char *dev, int baud)
{
    int fd, mcs;
    int hits = 0;

    /* Check modem line status first: if CTS/DSR are both 0 the chip is not online at all
     * (no power, wrong UART wiring, or pinmux not configured as UART). No data needs to be
     * sent, so this is the quietest test. */
    fd = open(dev, O_RDWR | O_NOCTTY | O_NONBLOCK);
    if (fd < 0) {
        printf("  %-16s open: %s\n", dev, strerror(errno));
        return -1;
    }
    if (ioctl(fd, TIOCMGET, &mcs) == 0) {
        printf("  %-16s line status: CTS=%d DSR=%d DCD=%d RNG=%d RTS=%d DTR=%d\n",
               dev,
               !!(mcs & TIOCM_CTS), !!(mcs & TIOCM_DSR), !!(mcs & TIOCM_CAR),
               !!(mcs & TIOCM_RNG), !!(mcs & TIOCM_RTS), !!(mcs & TIOCM_DTR));
    } else {
        printf("  %-16s TIOCMGET: %s\n", dev, strerror(errno));
    }
    close(fd);

    /* Try with and without flow control: the BCM BT UART needs RTS/CTS by default */
    hits |= uart_probe_once(dev, baud, 1);
    if (!hits)
        hits |= uart_probe_once(dev, baud, 0);
    return hits;
}

static void scan_uarts(void)
{
    static const char *devs[] = {
        "/dev/ttyTHS0", "/dev/ttyTHS1", "/dev/ttyTHS2", "/dev/ttyTHS3"
    };
    unsigned int i;
    int any = 0;

    printf("\n-- scanning BT UART (HCI Reset @115200, 600ms wait) --\n");
    for (i = 0; i < sizeof(devs) / sizeof(devs[0]); i++) {
        int r = uart_probe(devs[i], B115200);
        if (r > 0) any = 1;
    }
    printf("\nConclusion: %s\n", any ? "found a port that answers HCI events" :
           "no ttyTHS answered HCI - the chip side is down (power / UART wiring / pinmux)");
}


static const char *cap_name(unsigned int cap)
{
    static char buf[256];
    buf[0] = 0;
    if (cap & V4L2_CAP_RADIO)   strcat(buf, "RADIO ");
    if (cap & V4L2_CAP_TUNER)   strcat(buf, "TUNER ");
    if (cap & V4L2_CAP_AUDIO)   strcat(buf, "AUDIO ");
    if (cap & V4L2_CAP_RDS_CAPTURE) strcat(buf, "RDS ");
    if (cap & V4L2_CAP_HW_FREQ_SEEK) strcat(buf, "HW_SEEK ");
    if (cap & V4L2_CAP_VIDEO_CAPTURE) strcat(buf, "VIDEO_CAPTURE ");
    return buf;
}

static int show_caps(int fd)
{
    struct v4l2_capability vc;
    memset(&vc, 0, sizeof(vc));
    if (ioctl(fd, VIDIOC_QUERYCAP, &vc) < 0) {
        printf("  [FAIL] VIDIOC_QUERYCAP: %s\n", strerror(errno));
        return -1;
    }
    printf("  driver      : %s\n", vc.driver);
    printf("  card        : %s\n", vc.card);
    printf("  bus_info    : %s\n", vc.bus_info);
    printf("  capabilities: 0x%08x  (%s)\n", vc.capabilities, cap_name(vc.capabilities));

    if (!(vc.capabilities & V4L2_CAP_RADIO)) {
        printf("  [WARN] device does not declare V4L2_CAP_RADIO\n");
    }
    return 0;
}

static int show_tuner(int fd, const char *tag)
{
    struct v4l2_tuner vt;
    memset(&vt, 0, sizeof(vt));
    vt.index = 0;
    if (ioctl(fd, VIDIOC_G_TUNER, &vt) < 0) {
        printf("  [FAIL] VIDIOC_G_TUNER: %s\n", strerror(errno));
        return -1;
    }
    printf("  %s: name='%s' range=%u..%u kHz step_free signal=%u afc=%d "
           "cap=0x%x rxsubchans=0x%x audmode=%d\n",
           tag, vt.name, vt.rangelow / 16, vt.rangehigh / 16,
           vt.signal, vt.afc, vt.capability, vt.rxsubchans, vt.audmode);
    return (int)vt.signal;
}

static int set_freq(int fd, int khz)
{
    struct v4l2_frequency vf;
    memset(&vf, 0, sizeof(vf));
    vf.tuner    = 0;
    vf.type     = V4L2_TUNER_RADIO;
    vf.frequency = KHZ_TO_V4L2(khz);
    if (ioctl(fd, VIDIOC_S_FREQUENCY, &vf) < 0) {
        printf("  [FAIL] VIDIOC_S_FREQUENCY(%d kHz): %s\n", khz, strerror(errno));
        return -1;
    }
    return 0;
}

static int get_freq(int fd)
{
    struct v4l2_frequency vf;
    memset(&vf, 0, sizeof(vf));
    vf.tuner = 0;
    if (ioctl(fd, VIDIOC_G_FREQUENCY, &vf) < 0) {
        printf("  [FAIL] VIDIOC_G_FREQUENCY: %s\n", strerror(errno));
        return -1;
    }
    return vf.frequency / 16;
}

static int set_ctrl(int fd, unsigned int id, int value, const char *name)
{
    struct v4l2_control vctrl;
    memset(&vctrl, 0, sizeof(vctrl));
    vctrl.id    = id;
    vctrl.value = value;
    if (ioctl(fd, VIDIOC_S_CTRL, &vctrl) < 0) {
        printf("  [WARN] VIDIOC_S_CTRL(%s=%d): %s\n", name, value, strerror(errno));
        return -1;
    }
    printf("  set %-8s = %d  OK\n", name, value);
    return 0;
}

static void dump_controls(int fd)
{
    struct v4l2_queryctrl qc;
    printf("\n-- V4L2 controls --\n");
    memset(&qc, 0, sizeof(qc));
    qc.id = V4L2_CTRL_FLAG_NEXT_CTRL;
    while (ioctl(fd, VIDIOC_QUERYCTRL, &qc) == 0) {
        struct v4l2_control cur;
        memset(&cur, 0, sizeof(cur));
        cur.id = qc.id;
        if (ioctl(fd, VIDIOC_G_CTRL, &cur) < 0)
            cur.value = qc.default_value;
        printf("  ctrl 0x%08x %-20s min=%-6d max=%-6d step=%-4d cur=%d\n",
               qc.id, qc.name, qc.minimum, qc.maximum, qc.step, cur.value);
        qc.id |= V4L2_CTRL_FLAG_NEXT_CTRL;
    }
}

/* Common frequencies (kHz) covering the normal FM band */
static const int scan_list[] = {
    87500, 90500, 93000, 95000, 97400, 99000, 101300, 103900, 106100, 107900
};

/*
 * Read the FM driver debug sysfs (it only exists while /dev/radio0 is open; release calls
 * sysfs_remove_group). The most useful entries are:
 *   rssi_raw=0xNN  raw chip RSSI register (0x00~0xFF)
 *   snr=NN         raw chip SNR register
 *
 * Why raw matters: V4L2 tuner->signal is computed by the driver;
 *   signal = (128 - curr_rssi) * (65535 / 128)
 * if curr_rssi's type or sign is truncated along the way the result looks like
 * "signal present but perfectly flat" fake data. Only raw distinguishes a real station
 * from noise with no antenna.
 */
/* Diagnostic helper, not called by the main flow (antenna_probe inlines the raw parsing
 * to get raw values). Kept as a utility for manual debugging; marked unused. */
static __attribute__((unused)) int read_fm_sysfs(void)
{
    static const char *paths[] = {
        "/sys/class/video4linux/radio0/fmrx_audio_status",
        "/sys/class/video4linux/video0/fmrx_audio_status",
        "/sys/class/radio/radio0/fmrx_audio_status",
        "/sys/devices/platform/tegra-fm/fmrx_audio_status",
    };
    char buf[512];
    int i, fd, n;

    for (i = 0; i < (int)(sizeof(paths) / sizeof(paths[0])); i++) {
        fd = open(paths[i], O_RDONLY);
        if (fd < 0)
            continue;
        n = read(fd, buf, sizeof(buf) - 1);
        close(fd);
        if (n <= 0)
            continue;
        buf[n] = 0;
        printf("  sysfs %s\n        %s", paths[i], buf);
        return 0;
    }
    return -1;
}

/*
 * antenna_probe - decide whether "headphones as antenna" actually works.
 *
 * Idea: when a real station is received, raw RSSI shows clear peaks and valleys across
 * frequency (a spike at the station, noise floor at empty spots). If raw values over the
 * whole band only wobble in a narrow range, the RF front end has no antenna connected -
 * the values are chip-internal noise regardless of the headset.
 *
 * Prints signal and raw rssi/snr per frequency, then the peak-to-peak dynamic range.
 */
static void antenna_probe(int fd)
{
    int i, vals[64];
    int nr = 0, vmin = 1 << 30, vmax = 0;
    char line[256];

    printf("\n-- antenna/spectrum response probe (800ms per point, raw RSSI) --\n");
    printf("  %-8s %-8s %-10s %s\n", "freq", "signal", "raw", "snr");

    for (i = 0; i < (int)(sizeof(scan_list) / sizeof(scan_list[0])); i++) {
        struct v4l2_tuner vt;
        int sig;
        unsigned int raw = 0, snr = 0;
        int fd2;

        if (set_freq(fd, scan_list[i]) < 0)
            continue;
        /* 800ms lets the chip finish PLL lock and refresh the RSSI window; 350ms is too short */
        usleep(800 * 1000);

        memset(&vt, 0, sizeof(vt));
        vt.index = 0;
        sig = (ioctl(fd, VIDIOC_G_TUNER, &vt) == 0) ? (int)vt.signal : -1;

        /* Also grab the raw registers */
        fd2 = open("/sys/class/video4linux/radio0/fmrx_audio_status", O_RDONLY);
        if (fd2 >= 0) {
            int n = read(fd2, line, sizeof(line) - 1);
            close(fd2);
            if (n > 0) {
                char *p;
                line[n] = 0;
                p = strstr(line, "rssi_raw=");
                if (p) raw = (unsigned int)strtoul(p + 9, NULL, 0);
                p = strstr(line, "snr=");
                if (p) snr = (unsigned int)strtoul(p + 4, NULL, 0);
            }
        }

        printf("  %-8d %-8d 0x%02x(%-3u) %u\n", scan_list[i], sig, raw, raw, snr);

        if (sig > 0) {
            vals[nr < 64 ? nr : 63] = sig;
            if (sig < vmin) vmin = sig;
            if (sig > vmax) vmax = sig;
            nr++;
        }
    }

    if (nr > 1) {
        printf("\n  peak-to-peak: max=%d min=%d range=%d (%.1f%%)\n",
               vmax, vmin, vmax - vmin,
               vmin ? (double)(vmax - vmin) * 100.0 / vmin : 0.0);
        if (vmax - vmin < vmax / 5)
            printf("  verdict: spectrum almost flat - no effective antenna at the RF front end\n");
        else
            printf("  verdict: clear peaks and valleys - antenna chain works, real stations received\n");
    }
}

int main(int argc, char **argv)
{
    int fd, i, rc = 0;

    /* fmtest --uart : UART scan only, to find which tty the BT chip is on.
     * Does not touch /dev/radio0, so it works even when FM cannot be opened. */
    if (argc > 1 && strcmp(argv[1], "--uart") == 0) {
        scan_uarts();
        return 0;
    }

    printf("=== fmtest: %s ===\n", FM_DEV);

    fd = open(FM_DEV, O_RDWR | O_NONBLOCK);
    if (fd < 0) {
        printf("[FAIL] open(%s): %s\n", FM_DEV, strerror(errno));
        return 1;
    }
    printf("[ OK ] open %s -> fd=%d\n", FM_DEV, fd);

    printf("\n-- capability --\n");
    if (show_caps(fd) < 0) { rc = 2; goto out; }

    printf("\n-- tuner (state after power-on) --\n");
    show_tuner(fd, "G_TUNER");

    /* fmtest --antenna : antenna/spectrum probe only. Must be caught before atoi,
     * otherwise atoi turns "--antenna" into 0 and it tunes to 0 kHz. */
    if (argc > 1 && strcmp(argv[1], "--antenna") == 0) {
        antenna_probe(fd);
        goto out;
    }

    /* fmtest --hold <khz> <sec> : tune to a frequency and *keep the device open*,
     * printing signal once per second.
     *
     * Why this mode: once /dev/radio0 is closed, the driver's release path calls
     * fmc_turn_fm_off() and the chip stops, so audio stops immediately. To check whether
     * there is sound in the headphones the fd must stay open while tinymix sets up the codec.
     */
    if (argc > 3 && strcmp(argv[1], "--hold") == 0) {
        int khz = atoi(argv[2]);
        int sec = atoi(argv[3]);
        if (sec <= 0) sec = 30;

        set_ctrl(fd, V4L2_CID_AUDIO_MUTE, 0, "mute=off");
        set_ctrl(fd, V4L2_CID_AUDIO_VOLUME, 15, "volume");
        if (set_freq(fd, khz) < 0) { rc = 3; goto out; }
        usleep(600 * 1000);
        printf("\n-- holding for %d seconds (configure codec routing / put on headphones) --\n", sec);
        printf("  actual frequency: %d kHz\n", get_freq(fd));
        for (i = 0; i < sec; i++) {
            usleep(1000 * 1000);
            printf("  [%2ds] ", i + 1);
            fflush(stdout);
            show_tuner(fd, "sig");
        }
        goto out;
    }

    if (argc > 1) {
        int khz = atoi(argv[1]);
        printf("\n-- tuning to %d kHz --\n", khz);
        if (set_freq(fd, khz) < 0) { rc = 3; goto out; }
        usleep(400 * 1000);
        printf("  actual frequency: %d kHz\n", get_freq(fd));
        show_tuner(fd, "G_TUNER");
        goto out;
    }

    dump_controls(fd);

    printf("\n-- powering on the radio (volume/mute/tune) --\n");
    set_ctrl(fd, V4L2_CID_AUDIO_MUTE, 0, "mute=off");
    set_ctrl(fd, V4L2_CID_AUDIO_VOLUME, 10, "volume");

    printf("\n-- per-frequency sweep (check whether signal changes) --\n");
    printf("  %-8s %-8s %s\n", "freq", "got", "signal");
    for (i = 0; i < (int)(sizeof(scan_list) / sizeof(scan_list[0])); i++) {
        if (set_freq(fd, scan_list[i]) < 0) continue;
        /* wait for the chip to settle and RSSI to refresh */
        usleep(350 * 1000);
        printf("  %-8d %-8d %d\n", scan_list[i], get_freq(fd),
               show_tuner(fd, "sig"));
    }

    printf("\n-- hardware seek (VIDIOC_S_HW_FREQ_SEEK) --\n");
    {
        struct v4l2_hw_freq_seek seek;
        memset(&seek, 0, sizeof(seek));
        seek.tuner      = 0;
        seek.type       = V4L2_TUNER_RADIO;
        seek.seek_upward = 1;
        seek.wrap_around = 1;
        seek.rangelow   = KHZ_TO_V4L2(87500);
        seek.rangehigh  = KHZ_TO_V4L2(108000);
        if (ioctl(fd, VIDIOC_S_HW_FREQ_SEEK, &seek) < 0)
            printf("  [WARN] HW_SEEK unsupported or failed: %s\n", strerror(errno));
        else
            printf("  found station: %d kHz\n", get_freq(fd));
    }

out:
    close(fd);
    printf("\n=== done (rc=%d) ===\n", rc);
    return rc;
}
