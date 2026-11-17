/* French text as the BeSTspeech French builds need it.
 *
 * The 2006 French build reads every c, C and ç as s before its letter-to-
 * sound rules see the word: "comme" comes out as "somme", "aucun" as
 * "ausun", "école" as "ésole". The original DLL does the same, and its own
 * reference recordings say "ecole" that way, so the host software must have
 * spelled the text out for it. This does that spelling: a hard c becomes k,
 * a soft one s, or ss between vowels, where a single s would be read as z.
 *
 * Both French builds say "après" with a z whatever follows it.
 *
 * Checked against espeak-ng on the 68 000 words of the Hunspell French
 * dictionary: with this, the 2006 build disagrees with espeak-ng on 13 % of
 * them, as the 1998 build does, against 36 % without; what is left is mostly
 * open and closed o, the glide in "nuage", and borrowed words.
 */

#include <stddef.h>
#include <stdint.h>

#include "french.h"

static uint32_t lower(uint32_t c)
{
    if ((c >= 'A' && c <= 'Z') || (c >= 0xC0 && c <= 0xDE && c != 0xD7))
        return c + 32;
    if (c == 0x152)
        return 0x153;
    if (c == 0x178)
        return 0xFF;
    return c;
}

static int is_letter(uint32_t c)
{
    uint32_t l = lower(c);

    return (l >= 'a' && l <= 'z') || (l >= 0xDF && l <= 0xFF && l != 0xF7)
        || l == 0x153;
}

static int is_vowel(uint32_t c)
{
    switch (lower(c)) {
    case 'a': case 'e': case 'i': case 'o': case 'u': case 'y':
    case 0xE0: case 0xE2: case 0xE4: case 0xE6: case 0xE8: case 0xE9:
    case 0xEA: case 0xEB: case 0xEE: case 0xEF: case 0xF4: case 0xF6:
    case 0xF9: case 0xFB: case 0xFC: case 0xFF: case 0x153:
        return 1;
    default:
        return 0;
    }
}

/* The vowels before which c is s. */
static int is_front(uint32_t c)
{
    switch (lower(c)) {
    case 'e': case 'i': case 'y': case 0xE6: case 0xE8: case 0xE9:
    case 0xEA: case 0xEB: case 0xEE: case 0xEF: case 0xFF:
        return 1;
    default:
        return 0;
    }
}

/* Whether the word t[from..to), lowered, begins with the UTF-8 text; with
 * whole set, whether it is exactly that. *length is how many letters. */
static int word_matches(const uint32_t *t, size_t from, size_t to,
                        const char *text, int whole, size_t *length)
{
    const unsigned char *p = (const unsigned char *)text;
    size_t k = from;

    while (*p) {
        uint32_t c = *p++;

        if (c >= 0xC0) {
            int extra = c < 0xE0 ? 1 : 2;

            c &= extra == 1 ? 0x1F : 0x0F;
            while (extra-- && *p)
                c = (c << 6) | (*p++ & 0x3F);
        }
        if (k >= to || lower(t[k]) != c)
            return 0;
        k++;
    }
    if (length)
        *length = k - from;
    return !whole || k == to;
}

static int word_ends(const uint32_t *t, size_t from, size_t to,
                     const char *ascii)
{
    size_t n = 0;

    while (ascii[n])
        n++;
    return to - from >= n && word_matches(t, to - n, to, ascii, 1, NULL);
}

#define COUNT(a) (sizeof (a) / sizeof *(a))

/* Endings whose last c is not said: "blanc", "ajonc", "antitabac". */
static const char *const silentFinalC[] = {
    "almanach", "banc", "blanc", "broc", "caoutchouc", "clerc", "croc",
    "estomac", "flanc", "franc", "jonc", "porc", "tabac", "tronc",
};

/* Words whose final ct is not said. */
static const char *const silentFinalCt[] = {
    "aspect", "distinct", "instinct", "irrespect", "respect", "succinct",
    "suspect",
};

/* Words in a said "cent". With its c spelled s, "cent" is the engine's
 * verb ending ("ils sentent"): "pour cent" comes out as "pour s", "récent"
 * as "rés". These get "sant", which it reads right. A verb in "-cent", as
 * "ils commencent", keeps it. Words in "-escent" go with them, all but the
 * verb "acquiescent". */
static const char *const saidCent[] = {
    "accent", "cent", "décent", "indécent", "innocent", "munificent",
    "récent", "réticent",
};

/* Word beginnings where ch is k, from Greek. psych is k anywhere. */
static const char *const hardCh[] = {
    "archaï", "archange", "archéo", "chaos", "chaot", "chiro", "chol",
    "chor", "chœur", "écho", "orchestr",
};

static int any_word(const uint32_t *t, size_t from, size_t to,
                    const char *const *list, size_t count)
{
    for (size_t k = 0; k < count; k++)
        if (word_matches(t, from, to, list[k], 1, NULL))
            return 1;
    return 0;
}

static int any_ending(const uint32_t *t, size_t from, size_t to,
                      const char *const *list, size_t count)
{
    for (size_t k = 0; k < count; k++)
        if (word_ends(t, from, to, list[k]))
            return 1;
    return 0;
}

static void put_ascii(uint32_t *out, size_t *o, const char *text)
{
    while (*text)
        out[(*o)++] = (unsigned char)*text++;
}

/* The first letter of the next word, or 0 if a sentence mark or the end of
 * the text comes first. Spaces and index marks are passed over. */
static uint32_t next_word_letter(const uint32_t *in, size_t n, size_t i)
{
    for (; i < n; i++) {
        if (in[i] == ' ' || in[i] == 0xA0 || in[i] >= 0x110000)
            continue;
        return is_letter(in[i]) ? lower(in[i]) : 0;
    }
    return 0;
}

/* One c, or ç, at position i of the word in[start..end). */
static size_t spell_c(const uint32_t *in, size_t n, size_t i, size_t start,
                      size_t end, uint32_t *out, size_t *o)
{
    uint32_t c = in[i], l = lower(c);
    uint32_t prev = i > start ? lower(in[i - 1]) : 0;
    uint32_t next = i + 1 < end ? lower(in[i + 1]) : 0;
    uint32_t after = i + 2 < end ? lower(in[i + 2]) : 0;

    (void)n;
    if (end - start == 1) {
        /* The letter on its own, as a key or a character: its name. */
        put_ascii(out, o, l == 'c' ? "s\xE9" : "s\xE9 s\xE9" "dille");
        return 0;
    }
    if (l == 0xE7 || is_front(next)) {
        /* Soft. After an s the s is already there ("science", "descendre"),
           and only needs doubling between vowels. */
        if (prev == 's') {
            if (i >= start + 2 && is_vowel(in[i - 2]))
                out[(*o)++] = 's';
            return 0;
        }
        /* "actrice": the engine reads "trisse" as triz-s, "trysse" right. */
        if (prev == 'i' && i >= start + 3 && lower(in[i - 2]) == 'r'
            && lower(in[i - 3]) == 't')
            out[*o - 1] = 'y';
        /* "déss" is read as the prefix "dés", with a z: "décider" comes out
           as "dézsider". "dhéss" is read right. */
        if (prev == 0xE9 && i >= start + 2 && lower(in[i - 2]) == 'd') {
            out[*o] = out[*o - 1];
            out[*o - 1] = 'h';
            (*o)++;
        }
        out[(*o)++] = 's';
        if (prev && is_vowel(prev))
            out[(*o)++] = 's';
        return 0;
    }
    if (next == 'h') {
        /* ch is the engine's own sh, but k before a consonant ("chrome",
           "technique") and in words from Greek. */
        int greek = i >= start + 3 && prev == 'y' && lower(in[i - 2]) == 's'
                 && lower(in[i - 3]) == 'p';
        for (size_t k = 0; k < COUNT(hardCh) && !greek; k++) {
            size_t length;
            greek = word_matches(in, start, end, hardCh[k], 0, &length)
                 && start + length > i + 1;
        }
        if (greek || (after && !is_vowel(after) && after != 'h')) {
            out[(*o)++] = 'k';
            return 1;
        }
        /* A word in "déch" is read as one in "désh", like "déshabiller";
           "dhéch" is read right. */
        if (i == start + 2 && lower(in[start]) == 'd'
            && lower(in[start + 1]) == 0xE9) {
            out[*o] = out[*o - 1];
            out[*o - 1] = 'h';
            (*o)++;
        }
        out[(*o)++] = c;
        return 0;
    }
    if (next == 'c') {
        /* "accès" is ks, "accord" k. */
        out[(*o)++] = 'k';
        if (is_front(after))
            out[(*o)++] = 's';
        return 1;
    }
    if (next == 'q')
        return 0; /* "acquérir": the qu says it */
    if (next == 't' && end == i + 2) {
        /* A final ct is said, as in "direct", and the engine drops a lone t
           after k, so it gets two. */
        if (any_word(in, start, end, silentFinalCt, COUNT(silentFinalCt))) {
            out[(*o)++] = 't';
        } else {
            put_ascii(out, o, "ktt");
        }
        return 1;
    }
    if (end == i + 1) {
        if (!any_ending(in, start, end, silentFinalC, COUNT(silentFinalC)))
            out[(*o)++] = 'k';
        return 0;
    }
    if (i == start + 2 && word_matches(in, start, end, "second", 0, NULL)) {
        out[(*o)++] = 'g';
        return 0;
    }
    out[(*o)++] = 'k';
    return 0;
}

size_t french_respell(const uint32_t *in, size_t n, uint32_t *out,
                      int spellC)
{
    size_t o = 0, start = 0, end = 0;

    for (size_t i = 0; i < n; i++) {
        uint32_t c = in[i], l;

        if (!is_letter(c)) {
            out[o++] = c;
            continue;
        }
        if (i >= end) {
            start = i;
            for (end = i; end < n && is_letter(in[end]); end++)
                ;
        }
        l = lower(c);
        if (spellC && (l == 'c' || l == 0xE7)) {
            i += spell_c(in, n, i, start, end, out, &o);
            continue;
        }
        /* "après" ends in z only before a vowel. */
        if (l == 's' && i + 1 == end
            && word_matches(in, start, end, "apr\xC3\xA8s", 1, NULL)) {
            uint32_t next = next_word_letter(in, n, end);
            if (!is_vowel(next) && next != 'h')
                continue;
        }
        if (spellC && l == 'e' && i + 3 == end && i > start
            && lower(in[i - 1]) == 'c' && lower(in[i + 1]) == 'n'
            && lower(in[i + 2]) == 't'
            && (any_word(in, start, end, saidCent, COUNT(saidCent))
                || (word_ends(in, start, end, "escent")
                    && !word_ends(in, start, end, "acquiescent")))) {
            out[o++] = c == 'E' ? 'A' : 'a';
            continue;
        }
        out[o++] = c;
    }
    return o;
}
