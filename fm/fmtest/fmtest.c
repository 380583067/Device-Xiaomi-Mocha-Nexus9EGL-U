/*
 * fmtest - 小米平板1 (mocha) FM 芯片链路诊断工具
 *
 * 目的：绕开 Android 框架（FMRadio App / libfmjni / AudioManager 耳机检测），
 *       直接对 /dev/radio0 做标准 V4L2 radio ioctl，验证
 *         kernel v4l2_fm_driver  ->  brcm-uim-sysfs  ->  BCM4354 FM 芯片
 *       这条链路是否真正可用。
 *
 * 用法（root）：
 *   fmtest             # 完整诊断：QUERYCAP / G_TUNER / 调频 / 扫频 RSSI
 *   fmtest 97400       # 只调到指定频率 (kHz) 并读 RSSI
 *
 * 注意：不涉及音频通路（I2S -> codec），音频需要真机插耳机验证。
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

/* V4L2 radio 的频率单位是 62.5 Hz，即 1 kHz = 16 */
#define KHZ_TO_V4L2(khz) ((khz) * 16)

/* HCI Reset 命令（OGF=0x03 Host Control, OCF=0x0003）：01 03 0C 00 */
static const unsigned char hci_reset_cmd[] = { 0x01, 0x03, 0x0C, 0x00 };

/*
 * uart_probe - 往指定 tty 发一条 HCI Reset，看 BCM 芯片有没有回
 *              Command Complete (04 0E 04 01 03 0C 00)。
 *
 * 为什么要单独做这个：brcm-uim-sysfs 里串口名来自 bt_vendor.conf 的
 * UimUartPort。如果这个值和真机上 BT 实际连接的 UART 不一致，芯片永远
 * 不会有回应，uim 就会卡死在 read_hci_event，内核那边看到的就是
 *     line disc installation timed out
 *     Failed to get shared ldisc write func pointer
 * 直接扫一遍所有 ttyTHS 是最快的判定方式。
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

    /* 等最多 600ms 的回包 */
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
                printf("  %-16s [flow=%d] 收到 %d 字节:", dev, hwflow, r);
                for (k = 0; k < r; k++)
                    printf(" %02x", buf[k]);
                printf("%s\n", (buf[0] == 0x04) ? "   <== HCI event，就是它！" : "");
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

    printf("  %-16s [flow=%d] 无响应\n", dev, hwflow);
    close(fd);
    return 0;
}

static int uart_probe(const char *dev, int baud)
{
    int fd, mcs;
    int hits = 0;

    /* 先看调制解调器线状态：如果 CTS/DSR 全为 0，说明芯片根本没在线
     * （没供电、串口焊错、或 pinmux 没配成 UART）。这一步不需要发数据，
     * 是最"安静"的判定方法。 */
    fd = open(dev, O_RDWR | O_NOCTTY | O_NONBLOCK);
    if (fd < 0) {
        printf("  %-16s open: %s\n", dev, strerror(errno));
        return -1;
    }
    if (ioctl(fd, TIOCMGET, &mcs) == 0) {
        printf("  %-16s 线状态: CTS=%d DSR=%d DCD=%d RNG=%d RTS=%d DTR=%d\n",
               dev,
               !!(mcs & TIOCM_CTS), !!(mcs & TIOCM_DSR), !!(mcs & TIOCM_CAR),
               !!(mcs & TIOCM_RNG), !!(mcs & TIOCM_RTS), !!(mcs & TIOCM_DTR));
    } else {
        printf("  %-16s TIOCMGET: %s\n", dev, strerror(errno));
    }
    close(fd);

    /* 有流控 / 无流控各试一次：BCM 的 BT UART 默认要 RTS/CTS */
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

    printf("\n-- 扫描 BT UART（发 HCI Reset @115200，等 600ms）--\n");
    for (i = 0; i < sizeof(devs) / sizeof(devs[0]); i++) {
        int r = uart_probe(devs[i], B115200);
        if (r > 0) any = 1;
    }
    printf("\n结论：%s\n", any ? "找到能回 HCI 事件的串口" :
           "所有 ttyTHS 都无 HCI 响应 —— 芯片侧没有起来（供电/UART 连线/pinmux）");
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
        printf("  [WARN] 设备没有声明 V4L2_CAP_RADIO\n");
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

/* 常用频点（kHz），覆盖常规 FM 波段 */
static const int scan_list[] = {
    87500, 90500, 93000, 95000, 97400, 99000, 101300, 103900, 106100, 107900
};

/*
 * 读 FM 驱动的调试 sysfs（只有 /dev/radio0 处于 open 状态时才存在，
 * release 时会 sysfs_remove_group 掉）。其中最有用的是：
 *   rssi_raw=0xNN  芯片 RSSI 寄存器原始值（0x00~0xFF）
 *   snr=NN         芯片 SNR 寄存器原始值
 *
 * 为什么必须看 raw：V4L2 的 tuner->signal 是驱动算过的
 *   signal = (128 - curr_rssi) * (65535 / 128)
 * 一旦 curr_rssi 的类型/符号在中间被截断，算出来的值会变成"看起来
 * 有信号但完全平坦"的假数据。只有 raw 能区分"真收到台"和"没天线在
 * 读噪声"。
 */
/* 诊断用途，当前主流程没调用（antenna_probe 里为了取 raw 值用了 inline
 * 解析）。保留成工具函数，手动排障时可以直接搬用；加 unused 免掉编译警告。 */
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
 * antenna_probe - 判断"耳机当天线"到底有没有生效。
 *
 * 思路：真收到电台时，RSSI raw 会随频率出现明显的峰谷（电台位置一个
 * 尖峰，空频点掉到底噪）。如果扫整个波段 raw 值只在很小范围内抖动，
 * 说明 RF 前端根本没接上天线 —— 此时数值是芯片内部噪声，与插不插
 * 耳机无关。
 *
 * 输出每个频点的 signal 和 raw rssi/snr，最后给出峰谷差（dynamic range）。
 */
static void antenna_probe(int fd)
{
    int i, vals[64];
    int nr = 0, vmin = 1 << 30, vmax = 0;
    char line[256];

    printf("\n-- 天线/频谱响应探测（每点 800ms，读 raw RSSI） --\n");
    printf("  %-8s %-8s %-10s %s\n", "freq", "signal", "raw", "snr");

    for (i = 0; i < (int)(sizeof(scan_list) / sizeof(scan_list[0])); i++) {
        struct v4l2_tuner vt;
        int sig;
        unsigned int raw = 0, snr = 0;
        int fd2;

        if (set_freq(fd, scan_list[i]) < 0)
            continue;
        /* 800ms：给芯片完成 PLL 锁定 + RSSI 计数窗口刷新，350ms 偏短 */
        usleep(800 * 1000);

        memset(&vt, 0, sizeof(vt));
        vt.index = 0;
        sig = (ioctl(fd, VIDIOC_G_TUNER, &vt) == 0) ? (int)vt.signal : -1;

        /* 顺带把 raw 寄存器抓出来 */
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
        printf("\n  峰谷: max=%d min=%d 动态范围=%d (%.1f%%)\n",
               vmax, vmin, vmax - vmin,
               vmin ? (double)(vmax - vmin) * 100.0 / vmin : 0.0);
        if (vmax - vmin < vmax / 5)
            printf("  判定: 频谱近乎平坦 —— RF 前端没有有效天线输入\n");
        else
            printf("  判定: 存在明显峰谷 —— 天线链路有效，收到真实电台\n");
    }
}

int main(int argc, char **argv)
{
    int fd, i, rc = 0;

    /* fmtest --uart : 只做串口扫描，判断 BT 芯片挂在哪个 tty 上。
     * 不碰 /dev/radio0，所以可以在 FM 打不开时单独用。 */
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

    printf("\n-- tuner (power-on 后状态) --\n");
    show_tuner(fd, "G_TUNER");

    /* fmtest --antenna : 只跑天线/频谱响应探测。必须在 atoi 之前拦，
     * 否则 "--antenna" 会被 atoi 解成 0 走进"调频到 0 kHz"的分支。 */
    if (argc > 1 && strcmp(argv[1], "--antenna") == 0) {
        antenna_probe(fd);
        goto out;
    }

    /* fmtest --hold <khz> <sec> : 调到指定频点后**保持设备打开**，
     * 期间每秒打印一次 signal。
     *
     * 为什么要这个模式：FM 的 /dev/radio0 一旦 close，驱动 release 里会
     * fmc_turn_fm_off() 把芯片关掉，音频立刻停。想验证"耳机里到底有没有
     * 声音"，就必须让 fd 一直开着，外部再用 tinymix 配好 codec 路由去听。
     */
    if (argc > 3 && strcmp(argv[1], "--hold") == 0) {
        int khz = atoi(argv[2]);
        int sec = atoi(argv[3]);
        if (sec <= 0) sec = 30;

        set_ctrl(fd, V4L2_CID_AUDIO_MUTE, 0, "mute=off");
        set_ctrl(fd, V4L2_CID_AUDIO_VOLUME, 15, "volume");
        if (set_freq(fd, khz) < 0) { rc = 3; goto out; }
        usleep(600 * 1000);
        printf("\n-- 保持 %d 秒（现在去配 codec 路由 / 戴耳机听） --\n", sec);
        printf("  实际频率: %d kHz\n", get_freq(fd));
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
        printf("\n-- 调频到 %d kHz --\n", khz);
        if (set_freq(fd, khz) < 0) { rc = 3; goto out; }
        usleep(400 * 1000);
        printf("  实际频率: %d kHz\n", get_freq(fd));
        show_tuner(fd, "G_TUNER");
        goto out;
    }

    dump_controls(fd);

    printf("\n-- 打开收音机（音量/静音/调频） --\n");
    set_ctrl(fd, V4L2_CID_AUDIO_MUTE, 0, "mute=off");
    set_ctrl(fd, V4L2_CID_AUDIO_VOLUME, 10, "volume");

    printf("\n-- 逐频点扫描（看 signal 是否有变化） --\n");
    printf("  %-8s %-8s %s\n", "freq", "got", "signal");
    for (i = 0; i < (int)(sizeof(scan_list) / sizeof(scan_list[0])); i++) {
        if (set_freq(fd, scan_list[i]) < 0) continue;
        /* 等芯片稳定 + RSSI 更新 */
        usleep(350 * 1000);
        printf("  %-8d %-8d %d\n", scan_list[i], get_freq(fd),
               show_tuner(fd, "sig"));
    }

    printf("\n-- 硬件搜台 (VIDIOC_S_HW_FREQ_SEEK) --\n");
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
            printf("  [WARN] HW_SEEK 不支持或不成功: %s\n", strerror(errno));
        else
            printf("  搜到电台: %d kHz\n", get_freq(fd));
    }

out:
    close(fd);
    printf("\n=== done (rc=%d) ===\n", rc);
    return rc;
}
