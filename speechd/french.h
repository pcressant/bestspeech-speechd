#ifndef BESTSPEECH_FRENCH_H
#define BESTSPEECH_FRENCH_H

#include <stddef.h>
#include <stdint.h>

/* Rewrites French text, as code points, the way the French builds read it
 * right. With spellC, every c is spelled out for the 2006 build, which reads
 * them all as s. out needs room for 2 * n + 16 code points. Anything that is
 * not a letter passes unchanged, the index marks among them, and a mark
 * between two letters ends the word. Returns how many were written. */
size_t french_respell(const uint32_t *in, size_t n, uint32_t *out, int spellC);

#endif
