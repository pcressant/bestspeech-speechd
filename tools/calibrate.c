/* Measures how many samples a sentence comes to across openbst's native rate
   values, which is what the speech-dispatcher rate mapping is calibrated on. */
#include <stdio.h>
#include <stdlib.h>
#include "bst.h"

int main(int argc, char **argv) {
    const char *build = argc > 1 ? argv[1] : "1995";
    const char *text = "The quick brown fox jumps over the lazy dog, and then it runs away.";
    static const int rates[] = { -99, -95, -90, -85, -80, -75, -70, -60, -50,
                                 -25, 0, 25, 50, 100, 150, 200, 300 };
    bst *h = bst_open(build);
    if (!h) return 1;
    long base = bst_length(h, text);
    for (size_t i = 0; i < sizeof rates / sizeof rates[0]; i++) {
        bst_set(h, "rate", rates[i]);
        long n = bst_length(h, text);
        printf("%s rate %4d: %7ld samples, speed %.2f\n", build, rates[i], n,
               (double)base / (double)n);
    }
    bst_close(h);
    return 0;
}
