/*
 * i2cprobe - probe I2C devices by doing a zero-length / 1-byte read.
 * Small diagnostic tool for the mocha camera sensors.
 *
 * Usage: i2cprobe <bus> <addr> [addr ...]
 *   e.g. i2cprobe 2 0x10 0x36 0x0c
 *
 * A successful read (ACK) means a chip is responding at that address.
 */
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <sys/ioctl.h>
#include <linux/i2c.h>
#include <linux/i2c-dev.h>

int main(int argc, char **argv)
{
    int fd, i, a;
    char path[64];
    unsigned char buf[2];
    struct i2c_msg msgs[1];
    struct i2c_rdwr_ioctl_data data;

    if (argc < 3) {
        fprintf(stderr, "Usage: %s <bus> <addr> [addr ...]\n", argv[0]);
        return 1;
    }

    snprintf(path, sizeof(path), "/dev/i2c-%s", argv[1]);
    fd = open(path, O_RDWR);
    if (fd < 0) {
        fprintf(stderr, "open %s failed: %s\n", path, strerror(errno));
        return 1;
    }

    for (i = 2; i < argc; i++) {
        a = (int)strtol(argv[i], NULL, 0);
        /* Try a 1-byte read at offset 0 */
        msgs[0].addr = a;
        msgs[0].flags = I2C_M_RD;
        msgs[0].len = 1;
        msgs[0].buf = buf;
        data.msgs = msgs;
        data.nmsgs = 1;

        errno = 0;
        if (ioctl(fd, I2C_RDWR, &data) < 0) {
            printf("0x%02X: NO ACK (%s)\n", a, strerror(errno));
        } else {
            printf("0x%02X: ACK  value=0x%02X\n", a, buf[0]);
        }
        fflush(stdout);
        usleep(200000);
    }

    close(fd);
    return 0;
}
