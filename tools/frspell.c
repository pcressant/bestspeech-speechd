/* Reads UTF-8 lines and writes them back as the module hands them to the
 * 2006 French build: every c spelled out. For test/french.py. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "french.h"

static size_t decode(const unsigned char *s, uint32_t *out)
{
    size_t n = 0;

    while (*s) {
        uint32_t c = *s;
        int extra = c < 0x80 ? 0 : c < 0xE0 ? 1 : c < 0xF0 ? 2 : 3;

        c &= extra == 0 ? 0x7F : extra == 1 ? 0x1F : extra == 2 ? 0x0F : 0x07;
        s++;
        for (int k = 0; k < extra && (*s & 0xC0) == 0x80; k++)
            c = (c << 6) | (*s++ & 0x3F);
        out[n++] = c;
    }
    return n;
}

static void put(uint32_t c)
{
    if (c < 0x80) {
        putchar((int)c);
    } else if (c < 0x800) {
        putchar((int)(0xC0 | (c >> 6)));
        putchar((int)(0x80 | (c & 0x3F)));
    } else {
        putchar((int)(0xE0 | (c >> 12)));
        putchar((int)(0x80 | ((c >> 6) & 0x3F)));
        putchar((int)(0x80 | (c & 0x3F)));
    }
}

int main(void)
{
    static char line[4096];
    static uint32_t in[4096], out[8208];

    while (fgets(line, sizeof line, stdin)) {
        size_t n = decode((const unsigned char *)line, in);
        size_t m = french_respell(in, n, out, 1);

        for (size_t k = 0; k < m; k++)
            put(out[k]);
    }
    return 0;
}
