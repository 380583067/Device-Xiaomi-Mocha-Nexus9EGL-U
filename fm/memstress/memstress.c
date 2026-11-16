/* Anonymous-memory pressure tool for testing zram / kswapd / lmkd.
 * Usage: memstress <megabytes> <hold_seconds>
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

int main(int argc, char **argv)
{
    if (argc != 3) {
        fprintf(stderr, "usage: %s <megabytes> <hold_seconds>\n", argv[0]);
        return 2;
    }

    size_t mb = (size_t)atoi(argv[1]);
    int hold = atoi(argv[2]);
    size_t bytes = mb * 1024u * 1024u;
    char *p = malloc(bytes);
    if (!p) {
        printf("malloc(%zu MB) failed\n", mb);
        return 1;
    }

    /* Touch one byte per page so the pages are really backed by RAM. */
    for (size_t i = 0; i < bytes; i += 4096)
        p[i] = (char)(i & 0xff);

    printf("touched %zu MB of anonymous memory, holding %d seconds\n",
           mb, hold);
    fflush(stdout);

    for (int i = 0; i < hold; i++) {
        /* Re-read to keep the pages hot and resident. */
        volatile char sink = 0;
        for (size_t j = 0; j < bytes; j += 65536)
            sink ^= p[j];
        (void)sink;
        sleep(1);
    }

    free(p);
    printf("released\n");
    return 0;
}
