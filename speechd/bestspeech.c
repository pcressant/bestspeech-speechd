/* BeSTspeech / Keynote Gold output module for Speech Dispatcher.
 *
 * The engine is openbst, a native reimplementation of the Berkeley Speech
 * Technologies synthesizer. This executable reads Speech Dispatcher's module
 * protocol on standard input, synthesizes with openbst and hands 16-bit mono
 * PCM back to the server, which plays it. It opens no audio device.
 *
 * What comes from BstSpeech-sapi (the Windows SAPI5 wrapper around the
 * original b32_tts.dll): the fourteen voice presets, the text clean-up, and
 * the idea of handing the speeds the engine cannot reach to Sonic.
 *
 * Only the French and English builds are offered: 2006 and 1998 French,
 * 1995, 1998 and 2006 English.
 *
 * Responsiveness is the point of most of what follows. A screen reader puts
 * an index mark before every word; the module says a whole sentence in one
 * go and places the marks in its audio, instead of saying each word as a
 * sentence of its own. The silence the engine leaves before and after its
 * speech is cut down, and a message starts sounding as soon as its first
 * sentence is made, which takes well under a millisecond.
 */

/* strcasecmp, strtok_r and iconv are POSIX; the rest is C11. */
#define _POSIX_C_SOURCE 200809L

#include <ctype.h>
#include <iconv.h>
#include <math.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>

#include <speech-dispatcher/spd_module_main.h>

#include "french.h"

#include <sonic.h>

#include "bst.h"

/* ---- the builds and the presets ---------------------------------------- */

typedef struct {
    const char *build;    /* openbst's name for it */
    const char *tag;      /* the prefix of the voice names */
    const char *language; /* what Speech Dispatcher is told */
    const char *codeset;  /* what the build reads its text in */
    int commaEndsText;    /* says nothing after the first comma */
    int spellsC;          /* reads every c as s: see french.c */
    double gain;          /* brings its loudness into line with the others */
} BuildInfo;

/* The 2006 builds fold the Windows Western set into their own encoding. The
 * 1995 and 1998 ones read the DOS set: a Windows byte above 0x7f makes them
 * say nothing at all.
 *
 * The 2006 builds read a comma as the end of the text: "Bonjour, au revoir."
 * comes out as "Bonjour". A colon gives them the same phrase break without
 * ending anything, so a comma reaches them as one.
 *
 * Loudness, measured as the RMS of the voiced samples of a French sentence:
 * 1995 about 4900, the 1998 builds over 8000 and clipping, the 2006 builds
 * about 1360. The gains bring all of them to about 5000; espeak-ng gives
 * about 3100 at the same volume. */
static const BuildInfo builds[] = {
    { "2006FRE", "fr-2006", "fr", "CP1252", 1, 1, 3.7 },
    { "1998FRN", "fr-1998", "fr", "CP850",  0, 0, 0.6 },
    { "1995",    "en-1995", "en", "CP850",  0, 0, 1.0 },
    { "1998ENG", "en-1998", "en", "CP850",  0, 0, 0.6 },
    { "2006ENG", "en-2006", "en", "CP1252", 1, 0, 3.7 },
};
enum { BUILD_COUNT = sizeof builds / sizeof builds[0] };

/* BstSpeech-sapi's presets, which it wrote as the embedded commands
 * ~v ~e ~h ~u ~f. openbst's text reader does not take those commands, so they
 * are applied as settings: v is the voice, e the excitation, h the head room
 * above the floor, u the gain trim and f the floor in hertz. Fred is the
 * engine's own default. */
typedef struct {
    const char *name;
    int v, e, h, u, f;
} Preset;

static const Preset presets[] = {
    { "Fred",    0, 3,   0,   0,  80 },
    { "Sara",    2, 3, -20,   0, 175 },
    { "Hary",    3, 3,  10,   0,  65 },
    { "Wendy",   2, 1,  50,   0, 150 },
    { "Dexter",  6, 6,   0, -25,  90 },
    { "Alien",   4, 6, -50, -20, 115 },
    { "Kit",     5, 3,  40,   0, 230 },
    { "Bruno",   3, 3,  50,   0,  60 },
    { "Ghost",   3, 2,  50,   0,  60 },
    { "Peeper",  2, 2,   0,   5,  80 },
    { "Dracula", 3, 3,  45,  -5,  47 },
    { "Granny",  4, 3, -60,   0, 350 },
    { "Martha",  6, 4, 100,  -5, 300 },
    { "Tim",     3, 4, -10,   0,  60 },
};
enum { PRESET_COUNT = sizeof presets / sizeof presets[0] };

/* Speech Dispatcher's voice types, male1 to child_female, as presets. */
static const int voiceTypePreset[8] = {
    0,  /* male1: Fred */
    2,  /* male2: Hary */
    4,  /* male3: Dexter */
    1,  /* female1: Sara */
    3,  /* female2: Wendy */
    12, /* female3: Martha */
    13, /* child_male: Tim */
    6,  /* child_female: Kit */
};

/* ---- state --------------------------------------------------------------- */

typedef struct {
    int rate;
    int pitch;
    int pitchRange;
    int volume;
    SPDPunctuation punctuation;
    SPDSpelling spelling;
    SPDCapitalLetters capitals;
    SPDVoiceType voiceType;
    char *language;
    char *synthesisVoice;
} ModuleSettings;

static ModuleSettings settings = {
    .volume = 100,
    .punctuation = SPD_PUNCT_NONE,
    .spelling = SPD_SPELL_OFF,
    .capitals = SPD_CAP_NONE,
    .voiceType = SPD_MALE1,
};

static bst *engines[BUILD_COUNT];
static int engineLevel[BUILD_COUNT];
static int engineVoice[BUILD_COUNT];
static iconv_t strictConv[BUILD_COUNT];
static iconv_t translitConv[BUILD_COUNT];
static SPDVoice **voiceList;
static int defaultBuild[2] = { 0, 2 }; /* fr, en */
static double maxSpeed = 4.0;
static int rawOutput;      /* the engine's samples untouched, for the tests */
static volatile int stopRequested;
static volatile int pauseRequested;
static FILE *debugFile;
static int debugEnabled;
static int logLevel;

static void debug_log(const char *format, ...)
    __attribute__((format(printf, 1, 2)));

static void debug_log(const char *format, ...)
{
    va_list args;

    if (!debugEnabled)
        return;
    va_start(args, format);
    vfprintf(debugFile ? debugFile : stderr, format, args);
    va_end(args);
    fputc('\n', debugFile ? debugFile : stderr);
    fflush(debugFile ? debugFile : stderr);
}

static char *copy_string(const char *value)
{
    char *copy;

    if (!value || !strcmp(value, "NULL"))
        return NULL;
    copy = malloc(strlen(value) + 1);
    if (copy)
        strcpy(copy, value);
    return copy;
}

static char *copy_bytes(const char *value, size_t length)
{
    char *copy = malloc(length + 1);

    if (copy) {
        memcpy(copy, value, length);
        copy[length] = 0;
    }
    return copy;
}

static int parse_number(const char *value, int minimum, int maximum, int *out)
{
    char *end;
    long parsed = strtol(value, &end, 10);

    if (end == value || *end || parsed < minimum || parsed > maximum)
        return -1;
    *out = (int)parsed;
    return 0;
}

static int build_index(const char *name)
{
    for (int i = 0; i < BUILD_COUNT; i++)
        if (!strcasecmp(builds[i].build, name) || !strcasecmp(builds[i].tag, name))
            return i;
    return -1;
}

static int clamp(int value, int low, int high)
{
    return value < low ? low : value > high ? high : value;
}

/* ---- growable buffers ----------------------------------------------------- */

typedef struct {
    char *data;
    size_t length;
    size_t capacity;
} Buffer;

static int buffer_append(Buffer *b, const char *text, size_t length)
{
    if (b->length + length + 1 > b->capacity) {
        size_t capacity = b->capacity ? b->capacity : 128;
        char *data;

        while (capacity < b->length + length + 1)
            capacity *= 2;
        data = realloc(b->data, capacity);
        if (!data)
            return -1;
        b->data = data;
        b->capacity = capacity;
    }
    memcpy(b->data + b->length, text, length);
    b->length += length;
    b->data[b->length] = 0;
    return 0;
}

static int buffer_append_str(Buffer *b, const char *text)
{
    return buffer_append(b, text, strlen(text));
}

/* Code points, with the index marks among them as values above Unicode. */
typedef struct {
    uint32_t *v;
    size_t n, capacity;
} Points;

enum { MARK_BASE = 0x200000 };

static int points_push(Points *p, uint32_t c)
{
    if (p->n == p->capacity) {
        size_t capacity = p->capacity ? p->capacity * 2 : 256;
        uint32_t *grown = realloc(p->v, capacity * sizeof *grown);

        if (!grown)
            return -1;
        p->v = grown;
        p->capacity = capacity;
    }
    p->v[p->n++] = c;
    return 0;
}

/* The names of this message's index marks. */
static char **markNames;
static size_t markNameCount, markNameCapacity;

static void clear_mark_names(void)
{
    for (size_t i = 0; i < markNameCount; i++)
        free(markNames[i]);
    markNameCount = 0;
}

static int add_mark_name(const char *name, size_t length, uint32_t *code)
{
    if (markNameCount == markNameCapacity) {
        size_t capacity = markNameCapacity ? markNameCapacity * 2 : 32;
        char **grown = realloc(markNames, capacity * sizeof *grown);

        if (!grown)
            return -1;
        markNames = grown;
        markNameCapacity = capacity;
    }
    if (!(markNames[markNameCount] = copy_bytes(name, length)))
        return -1;
    *code = MARK_BASE + (uint32_t)markNameCount++;
    return 0;
}

/* ---- the queue one message becomes --------------------------------------- */

typedef struct {
    size_t offset; /* in the engine text, where the mark stands */
    char *name;
} PieceMark;

enum { ITEM_TEXT, ITEM_ICON };
enum { END_NONE, END_PHRASE, END_SENTENCE };

typedef struct {
    int kind;
    char *text;          /* engine text, NULL for marks alone; icon name */
    size_t length;
    PieceMark *marks;
    size_t markCount;
    int ending;          /* how the piece ends, for the pause after it */
} Item;

static Item *items;
static size_t itemCount, itemCapacity;

static void clear_items(void)
{
    for (size_t i = 0; i < itemCount; i++) {
        free(items[i].text);
        for (size_t k = 0; k < items[i].markCount; k++)
            free(items[i].marks[k].name);
        free(items[i].marks);
    }
    itemCount = 0;
}

static Item *new_item(int kind)
{
    if (itemCount == itemCapacity) {
        size_t capacity = itemCapacity ? itemCapacity * 2 : 16;
        Item *grown = realloc(items, capacity * sizeof *items);

        if (!grown)
            return NULL;
        items = grown;
        itemCapacity = capacity;
    }
    memset(&items[itemCount], 0, sizeof items[itemCount]);
    items[itemCount].kind = kind;
    return &items[itemCount++];
}

static int add_icon(const char *name, size_t length)
{
    Item *item = new_item(ITEM_ICON);

    if (!item || !(item->text = copy_bytes(name, length)))
        return -1;
    return 0;
}

/* The piece being built: engine text and the marks that fall in it. */
typedef struct {
    Buffer text;
    PieceMark *marks;
    size_t markCount, markCapacity;
} PieceBuilder;

static int piece_mark(PieceBuilder *p, const char *name)
{
    if (p->markCount == p->markCapacity) {
        size_t capacity = p->markCapacity ? p->markCapacity * 2 : 16;
        PieceMark *grown = realloc(p->marks, capacity * sizeof *grown);

        if (!grown)
            return -1;
        p->marks = grown;
        p->markCapacity = capacity;
    }
    p->marks[p->markCount].offset = p->text.length;
    if (!(p->marks[p->markCount].name = copy_string(name)))
        return -1;
    p->markCount++;
    return 0;
}

/* Ends the piece being built. Text that is nothing but spaces is not said:
 * the 2006 builds say an empty text as most of a second of silence. */
static int piece_flush(PieceBuilder *p, int ending)
{
    size_t i;
    int blank;
    Item *item;

    for (i = 0; i < p->text.length && p->text.data[i] == ' '; i++)
        ;
    blank = i == p->text.length;
    if (blank && !p->markCount)
        return 0;
    if (!(item = new_item(ITEM_TEXT)))
        return -1;
    if (!blank) {
        if (!(item->text = copy_bytes(p->text.data, p->text.length)))
            return -1;
        item->length = p->text.length;
    }
    item->marks = p->marks;
    item->markCount = p->markCount;
    item->ending = ending;
    p->marks = NULL;
    p->markCount = p->markCapacity = 0;
    p->text.length = 0;
    if (p->text.data)
        p->text.data[0] = 0;
    return 0;
}

static void piece_free(PieceBuilder *p)
{
    free(p->text.data);
    for (size_t i = 0; i < p->markCount; i++)
        free(p->marks[i].name);
    free(p->marks);
}

/* ---- text ------------------------------------------------------------------ */

/* Decodes one UTF-8 character; a byte that does not start a valid one is
 * taken as itself from Latin-1, so nothing is silently lost. */
static uint32_t utf8_next(const unsigned char **at)
{
    const unsigned char *p = *at;
    uint32_t c = p[0];
    int extra;

    if (c < 0x80) { *at = p + 1; return c; }
    if ((c & 0xE0) == 0xC0) { extra = 1; c &= 0x1F; }
    else if ((c & 0xF0) == 0xE0) { extra = 2; c &= 0x0F; }
    else if ((c & 0xF8) == 0xF0) { extra = 3; c &= 0x07; }
    else { *at = p + 1; return p[0]; }
    for (int i = 1; i <= extra; i++) {
        if ((p[i] & 0xC0) != 0x80) { *at = p + 1; return p[0]; }
        c = (c << 6) | (p[i] & 0x3F);
    }
    *at = p + 1 + extra;
    return c;
}

static int decode_utf8(Points *out, const char *text, size_t length)
{
    const unsigned char *at = (const unsigned char *)text;
    const unsigned char *end = at + length;

    while (at < end && *at)
        if (points_push(out, utf8_next(&at)))
            return -1;
    return 0;
}

static void utf8_put(Buffer *b, uint32_t c)
{
    char out[4];
    size_t n;

    if (c < 0x80) { out[0] = (char)c; n = 1; }
    else if (c < 0x800) {
        out[0] = (char)(0xC0 | (c >> 6));
        out[1] = (char)(0x80 | (c & 0x3F));
        n = 2;
    } else if (c < 0x10000) {
        out[0] = (char)(0xE0 | (c >> 12));
        out[1] = (char)(0x80 | ((c >> 6) & 0x3F));
        out[2] = (char)(0x80 | (c & 0x3F));
        n = 3;
    } else {
        out[0] = (char)(0xF0 | (c >> 18));
        out[1] = (char)(0x80 | ((c >> 12) & 0x3F));
        out[2] = (char)(0x80 | ((c >> 6) & 0x3F));
        out[3] = (char)(0x80 | (c & 0x3F));
        n = 4;
    }
    buffer_append(b, out, n);
}

static int is_letter_or_digit(uint32_t c)
{
    return (c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z')
        || (c >= 'a' && c <= 'z') || c == 0xAA || c == 0xB5 || c == 0xBA
        || (c >= 0xC0 && c <= 0x24F && c != 0xD7 && c != 0xF7)
        || (c >= 0x370 && c <= 0x52F);
}

static int is_digit(uint32_t c)
{
    return c >= '0' && c <= '9';
}

static int is_upper(uint32_t c)
{
    return (c >= 'A' && c <= 'Z') || (c >= 0xC0 && c <= 0xDE && c != 0xD7)
        || c == 0x152 || c == 0x160 || c == 0x178 || c == 0x17D;
}

/* What punctuation mode none drops: what Speech Dispatcher has already put a
 * name in for, while the marks that shape a sentence stay. */
static int is_droppable_symbol(uint32_t c)
{
    if (c == ',' || c == '.' || c == ';' || c == ':' || c == '?' || c == '!')
        return 0;
    return (c >= '!' && c <= '/') || (c >= ':' && c <= '@')
        || (c >= '[' && c <= '`') || (c >= '{' && c <= '~')
        || (c >= 0xA1 && c <= 0xBF && c != 0xAA && c != 0xB5 && c != 0xBA)
        || c == 0xD7 || c == 0xF7
        || (c >= 0x2010 && c <= 0x2BFF) || c == 0x20AC;
}

/* BstSpeech-sapi's clean-up, as code points: the typographic marks become the
 * plain ones the engine knows, line breaks and other control characters
 * become spaces, and so do the no-break spaces French typography puts before
 * its marks and inside its numbers. A tilde is dropped: it is the engine's
 * own command lead-in, and a tilde and bar in the text would end the phrase.
 * An ellipsis comes back as a full stop, with *ellipsis set. */
static uint32_t normalize(uint32_t c, int *ellipsis)
{
    if (ellipsis)
        *ellipsis = 0;
    switch (c) {
    case 0x2026:
        if (ellipsis)
            *ellipsis = 1;
        return '.';
    case 0x2010: case 0x2011: case 0x2012: case 0x2013: case 0x2014:
    case 0x2015: case 0x2212:
        return '-';
    case 0x2018: case 0x2019: case 0x201A: case 0x201B: case 0x2032:
        return '\'';
    case 0x201C: case 0x201D: case 0x201E: case 0x201F: case 0x2033:
        return '"';
    case 0xA0: case 0x2007: case 0x2009: case 0x200A: case 0x202F:
    case 0x2028: case 0x2029:
        return ' ';
    case '~':
        return ' ';
    case 0xAD: case 0x200B: case 0x200C: case 0x200D: case 0xFEFF:
        return 0;
    default:
        break;
    }
    if (c < 0x20 || c == 0x7F || (c >= 0x80 && c < 0xA0))
        return ' ';
    return c;
}

/* The next character after position i that is neither a mark nor dropped,
 * normalized; a space at the end of the text. *where is its position. */
static uint32_t peek_char(const Points *p, size_t i, size_t *where)
{
    for (; i < p->n; i++) {
        uint32_t c;

        if (p->v[i] >= MARK_BASE)
            continue;
        if ((c = normalize(p->v[i], NULL)) != 0) {
            if (where)
                *where = i;
            return c;
        }
    }
    if (where)
        *where = p->n;
    return ' ';
}

/* The same, past any spaces. */
static uint32_t peek_nonspace(const Points *p, size_t i, size_t *where)
{
    size_t at = i;
    uint32_t c;

    while ((c = peek_char(p, at, &at)) == ' ' && at < p->n)
        at++;
    if (where)
        *where = at;
    return c;
}

/* How many digits run from position i: three after a space is the shape of
 * a French thousands group. */
static size_t digit_run(const Points *p, size_t i)
{
    size_t n = 0, at = i;

    for (;;) {
        uint32_t c = peek_char(p, at, &at);

        if (!is_digit(c))
            return n;
        n++;
        at++;
    }
}

/* One code point into the build's code set: strictly if it has the
 * character, by transliteration if not, and as a space if neither works, so
 * that it is not read out as "question mark". */
static void encode_char(int b, uint32_t c, Buffer *out)
{
    Buffer utf8 = { 0 };
    char result[16];
    iconv_t convs[2] = { strictConv[b], translitConv[b] };

    if (c < 0x80) {
        char ch = (char)c;
        buffer_append(out, &ch, 1);
        return;
    }
    utf8_put(&utf8, c);
    for (int k = 0; k < 2; k++) {
        char *in = utf8.data, *res = result;
        size_t inLeft = utf8.length, outLeft = sizeof result;

        if (convs[k] == (iconv_t)-1)
            continue;
        iconv(convs[k], NULL, NULL, NULL, NULL);
        if (iconv(convs[k], &in, &inLeft, &res, &outLeft) == (size_t)-1
            || inLeft)
            continue;
        size_t n = sizeof result - outLeft;
        if (n == 0 || (k == 1 && memchr(result, '?', n)))
            continue;
        buffer_append(out, result, n);
        free(utf8.data);
        return;
    }
    free(utf8.data);
    buffer_append(out, " ", 1);
}

static int is_french(int b)
{
    return !strcmp(builds[b].language, "fr");
}

static const char *word_for(int b, const char *english, const char *french)
{
    return is_french(b) ? french : english;
}

/* Turns code points into engine text pieces, one to a sentence: a long text
 * starts sounding as soon as its first sentence is made, and a stop never
 * waits on more than one. The marks stay inside the pieces.
 *
 * Numbers need help. In every build two numbers with one space between them
 * make the whole sentence silent ("10 30", "1 2"), and so does a comma
 * between digits ("3,5"). Two spaces are safe; a comma between digits becomes
 * "virgule" in French; a group of three digits after one space is the French
 * way of writing thousands and is joined to the number before it. */
static int queue_points(int b, const Points *p, int suppressPunctuation,
                        int spell)
{
    PieceBuilder piece = { 0 };
    uint32_t previous = ' ', beforeSpace = ' ';
    size_t sinceBreak = 0;
    int result = 0;
    int inNumberGap = 0;

    buffer_append(&piece.text, "", 0);
    for (size_t i = 0; i < p->n && result == 0; i++) {
        int ellipsis, splitAfter = 0;
        uint32_t c, next;
        size_t nextAt;

        if (p->v[i] >= MARK_BASE) {
            result = piece_mark(&piece, markNames[p->v[i] - MARK_BASE]);
            continue;
        }
        if (!(c = normalize(p->v[i], &ellipsis)))
            continue;
        next = peek_char(p, i + 1, &nextAt);

        if (suppressPunctuation && !spell && is_droppable_symbol(c)
            && !(c == '\'' && is_letter_or_digit(previous)
                 && is_letter_or_digit(next)))
            c = ' ';

        if (inNumberGap) {
            if (c == ' ')
                continue;
            inNumberGap = 0;
        }

        if (spell) {
            if (c == ' ') {
                buffer_append(&piece.text, " ", 1);
            } else {
                if (settings.capitals != SPD_CAP_NONE && is_upper(c)) {
                    if (settings.capitals == SPD_CAP_SPELL) {
                        buffer_append_str(&piece.text, " ");
                        buffer_append_str(&piece.text,
                                          word_for(b, "capital", "majuscule"));
                        buffer_append_str(&piece.text, " ");
                    } else {
                        result = piece_flush(&piece, END_NONE);
                        if (result == 0)
                            result = add_icon("capital", 7);
                    }
                }
                uint32_t lc = c == 'C' ? 'c' : c == 0xC7 ? 0xE7 : c;
                if (builds[b].spellsC && (lc == 'c' || lc == 0xE7)) {
                    /* The build would say the letter as s. */
                    encode_char(b, 's', &piece.text);
                    encode_char(b, 0xE9, &piece.text);
                    if (lc == 0xE7)
                        buffer_append_str(&piece.text, " s\xe9" "dille");
                } else if (c == ',' && builds[b].commaEndsText)
                    buffer_append_str(&piece.text, word_for(b, "comma", "virgule"));
                else
                    encode_char(b, c, &piece.text);
                buffer_append(&piece.text, " ", 1);
            }
        } else if (c == ' ' && is_digit(previous)) {
            /* Spaces between two numbers. */
            uint32_t after = peek_nonspace(p, i, &nextAt);
            size_t spaces = 0;

            for (size_t k = i; k < nextAt; k++)
                if (p->v[k] < MARK_BASE && normalize(p->v[k], NULL) == ' ')
                    spaces++;
            if (!is_digit(after)) {
                buffer_append(&piece.text, " ", 1);
            } else {
                size_t run = digit_run(p, nextAt);
                if (!(spaces == 1 && run == 3))
                    buffer_append(&piece.text, "  ", 2);
                inNumberGap = 1;
            }
        } else if ((c == ',' || c == ':') && is_digit(next)
                   && (is_digit(previous)
                       || (previous == ' ' && is_digit(beforeSpace)))) {
            /* A comma or colon between digits. Speech Dispatcher's symbol
               pass writes 1,5 as "1 ,5". */
            size_t run = digit_run(p, nextAt);
            if (c == ',' && is_french(b))
                buffer_append_str(&piece.text, " virgule ");
            else if (!(c == ',' && run == 3))
                buffer_append(&piece.text, "  ", 2);
        } else if (ellipsis) {
            buffer_append(&piece.text, "...", 3);
        } else if (c == ',' && builds[b].commaEndsText) {
            buffer_append(&piece.text, ":", 1);
        } else {
            encode_char(b, c, &piece.text);
            splitAfter = (c == '.' || c == '?' || c == '!' || c == ';'
                          || c == ':') && next == ' ';
        }
        sinceBreak++;

        /* A sentence ends at its mark when a space follows; a run with no
           mark at all is broken at a space once it is long. */
        if (splitAfter && !spell) {
            result = result ? result : piece_flush(&piece, END_SENTENCE);
            sinceBreak = 0;
        } else if (c == ' ' && sinceBreak > 300) {
            result = result ? result : piece_flush(&piece, END_PHRASE);
            sinceBreak = 0;
        }
        if (c != ' ')
            beforeSpace = c;
        previous = c;
    }
    if (result == 0)
        result = piece_flush(&piece, END_NONE);
    piece_free(&piece);
    return result;
}

/* French text respelled for the build (see french.c), in place. Spelled
 * text is left alone: it is said a letter at a time. */
static int respell(int b, Points *p, int spell)
{
    uint32_t *out;

    if (spell || !is_french(b) || p->n == 0)
        return 0;
    size_t capacity = 2 * p->n + 16;
    if (!(out = malloc(capacity * sizeof *out)))
        return -1;
    p->n = french_respell(p->v, p->n, out, builds[b].spellsC);
    free(p->v);
    p->v = out;
    p->capacity = capacity;
    return 0;
}

static int queue_text(int b, const char *text, int suppressPunctuation,
                      int spell)
{
    Points p = { 0 };
    int result = decode_utf8(&p, text, strlen(text));

    if (result == 0)
        result = respell(b, &p, spell);
    if (result == 0)
        result = queue_points(b, &p, suppressPunctuation, spell);
    free(p.v);
    return result;
}

/* ---- SSML ----------------------------------------------------------------- */

static int tag_matches(const char *tag, size_t length, const char *name)
{
    size_t n = strlen(name);
    char next;

    if (length < n + 2 || tag[0] != '<' || tag[1] == '/')
        return 0;
    if (strncasecmp(tag + 1, name, n))
        return 0;
    next = tag[n + 1];
    return next == '>' || next == '/' || isspace((unsigned char)next);
}

static const char *find_attribute(const char *tag, size_t length,
                                  const char *attribute, size_t *valueLength)
{
    const char *end = tag + length;
    size_t n = strlen(attribute);

    for (const char *at = tag + 1; at + n < end; at++) {
        const char *value;
        char quote;

        if (!isspace((unsigned char)at[-1]) || strncasecmp(at, attribute, n))
            continue;
        value = at + n;
        while (value < end && isspace((unsigned char)*value))
            value++;
        if (value >= end || *value != '=')
            continue;
        value++;
        while (value < end && isspace((unsigned char)*value))
            value++;
        if (value >= end || (*value != '\'' && *value != '"'))
            continue;
        quote = *value++;
        const char *close = memchr(value, quote, (size_t)(end - value));
        if (!close)
            return NULL;
        *valueLength = (size_t)(close - value);
        return value;
    }
    return NULL;
}

static int push_entity(Points *out, const char *entity, size_t length)
{
    static const struct { const char *name; uint32_t value; } entities[] = {
        { "amp", '&' }, { "lt", '<' }, { "gt", '>' },
        { "quot", '"' }, { "apos", '\'' }, { "nbsp", 0xA0 },
    };

    if (length > 1 && entity[0] == '#') {
        long c = entity[1] == 'x' || entity[1] == 'X'
               ? strtol(entity + 2, NULL, 16) : strtol(entity + 1, NULL, 10);
        return points_push(out, c > 0 && c < 0x110000 ? (uint32_t)c : ' ');
    }
    for (size_t i = 0; i < sizeof entities / sizeof entities[0]; i++)
        if (strlen(entities[i].name) == length
            && !strncmp(entity, entities[i].name, length))
            return points_push(out, entities[i].value);
    return points_push(out, ' ');
}

static int contains_markup(const char *text)
{
    const char *tag = text;

    while ((tag = strchr(tag, '<')) != NULL) {
        const char *end = strchr(tag, '>');

        if (!end)
            return 0;
        size_t length = (size_t)(end - tag + 1);
        if (tag_matches(tag, length, "speak") || tag_matches(tag, length, "mark")
            || tag_matches(tag, length, "break"))
            return 1;
        tag = end + 1;
    }
    return 0;
}

/* SSML into code points: marks become mark values in the text, breaks a full
 * stop, other tags nothing, entities their characters. */
static int parse_ssml(Points *out, const char *data)
{
    const char *at = data;
    int result = 0;

    while (*at && result == 0) {
        if (*at == '<') {
            const char *end = strchr(at, '>');

            if (!end)
                break;
            size_t length = (size_t)(end - at + 1);
            if (tag_matches(at, length, "mark")) {
                size_t nameLength;
                const char *name = find_attribute(at, length, "name",
                                                  &nameLength);
                uint32_t code;

                if (name) {
                    result = add_mark_name(name, nameLength, &code);
                    if (result == 0)
                        result = points_push(out, code);
                }
            } else if (tag_matches(at, length, "break")) {
                result = points_push(out, '.');
                if (result == 0)
                    result = points_push(out, ' ');
            }
            at = end + 1;
        } else if (*at == '&') {
            const char *end = strchr(at + 1, ';');

            if (end && end - at < 12) {
                result = push_entity(out, at + 1, (size_t)(end - at - 1));
                at = end + 1;
            } else {
                result = points_push(out, (unsigned char)*at++);
            }
        } else {
            const char *end = strpbrk(at, "<&");
            size_t length = end ? (size_t)(end - at) : strlen(at);

            result = decode_utf8(out, at, length);
            at += length;
        }
    }
    return result;
}

/* ---- messages --------------------------------------------------------------- */

static char *key_text(int b, const char *data)
{
    Buffer text = { 0 };
    const char *at = data;

    buffer_append(&text, "", 0);
    if (!strcmp(data, " ") || !strcasecmp(data, "space")) {
        buffer_append_str(&text, word_for(b, "space", "espace"));
        return text.data;
    }
    if (!strncasecmp(at, "KP_", 3) || !strncasecmp(at, "KP-", 3)) {
        buffer_append_str(&text, word_for(b, "keypad ", "pav\xc3\xa9 num\xc3\xa9rique "));
        at += 3;
    }
    for (; *at; at++) {
        char c = *at == '_' || *at == '-' ? ' ' : *at;
        buffer_append(&text, &c, 1);
    }
    return text.data;
}

/* Which build and preset the settings in force ask for. */
static void resolve_voice(int *buildOut, int *presetOut)
{
    int b = -1, p = -1;

    if (settings.synthesisVoice) {
        for (int i = 0; i < BUILD_COUNT && b < 0; i++) {
            size_t n = strlen(builds[i].tag);
            if (strncasecmp(settings.synthesisVoice, builds[i].tag, n))
                continue;
            const char *rest = settings.synthesisVoice + n;
            if (!*rest) {
                b = i;
            } else if (*rest == '-') {
                for (int k = 0; k < PRESET_COUNT; k++)
                    if (!strcasecmp(rest + 1, presets[k].name)) {
                        b = i;
                        p = k;
                    }
            }
        }
    }
    if (b < 0) {
        int french = settings.language
                  && !strncasecmp(settings.language, "fr", 2)
                  && (!settings.language[2] || settings.language[2] == '-'
                      || settings.language[2] == '_');
        b = defaultBuild[french ? 0 : 1];
    }
    if (p < 0) {
        int type = (int)settings.voiceType - 1;
        p = type >= 0 && type < 8 ? voiceTypePreset[type] : 0;
    }
    *buildOut = b;
    *presetOut = p;
}

/* openbst's rate scales the durations, but not in proportion, and below -75
 * it stops getting faster. These are the speeds it reaches, measured on the
 * five builds with tools/calibrate.c; between them it is interpolated. */
static const struct { int native; double speed; } rateTable[] = {
    { 300, 0.28 }, { 200, 0.37 }, { 150, 0.43 }, { 100, 0.53 },
    { 50, 0.70 }, { 25, 0.82 }, { 0, 1.00 }, { -25, 1.29 },
    { -50, 1.75 }, { -60, 2.05 }, { -70, 2.38 }, { -75, 2.56 },
};
enum { RATE_POINTS = sizeof rateTable / sizeof rateTable[0] };

/* Speech Dispatcher's -100..100 as a speed: a quarter at -100, the engine's
 * own at 0, MaxSpeed at 100. Whatever the engine cannot reach is left to
 * Sonic, as BstSpeech-sapi did. Returns the speed. */
static double rate_to_engine(int rate, int *native, double *sonic)
{
    double speed = rate >= 0 ? pow(maxSpeed, rate / 100.0)
                             : pow(2.0, rate / 50.0);

    *sonic = 1.0;
    *native = 0;
    if (rate == 0)
        return 1.0;
    if (speed <= rateTable[0].speed) {
        *native = rateTable[0].native;
        *sonic = speed / rateTable[0].speed;
        return speed;
    }
    if (speed >= rateTable[RATE_POINTS - 1].speed) {
        *native = rateTable[RATE_POINTS - 1].native;
        *sonic = speed / rateTable[RATE_POINTS - 1].speed;
        return speed;
    }
    for (int i = 1; i < RATE_POINTS; i++) {
        if (speed <= rateTable[i].speed) {
            double a = log(rateTable[i - 1].speed), z = log(rateTable[i].speed);
            double t = (log(speed) - a) / (z - a);
            *native = (int)lround(rateTable[i - 1].native
                                  + t * (rateTable[i].native - rateTable[i - 1].native));
            break;
        }
    }
    return speed;
}

/* Puts the preset and the relative settings on the build's handle. */
static void configure(int b, int p, double *sonicSpeed, double *speed)
{
    const Preset *pr = &presets[p];
    bst *h = engines[b];
    double factor = pow(2.0, settings.pitch / 100.0);
    int floor = clamp((int)lround(pr->f * factor), 43, 600);
    /* The ceiling is the preset's head room above the floor, and the pitch
       range widens or narrows that room: the engine's own level setting does
       not move the contour of an ordinary sentence. */
    double room = (pr->h + 100) * pr->f / 100.0 * factor
                * pow(2.0, settings.pitchRange / 100.0);
    int top = clamp((int)lround(floor + room), 43, 600);
    int native = 0;

    /* v0 is the build's own voice: 1995 starts on one, the others on nought. */
    bst_set(h, "voice", pr->v > 0 && pr->v < 7 ? pr->v : engineVoice[b]);
    bst_set(h, "excitation", pr->e >= 1 ? pr->e * 0x10 : 0x30);
    bst_set(h, "gain", pr->u > -0x47 && pr->u < 0x15 ? pr->u - 0x12 : -0x12);
    bst_set(h, "pitch", floor);
    bst_set(h, "top", top < floor ? floor : top);
    bst_set(h, "level", engineLevel[b]);
    *speed = rate_to_engine(settings.rate, &native, sonicSpeed);
    bst_set(h, "rate", native);
    debug_log("build %s preset %s floor %d top %d rate %d sonic %.2f",
              builds[b].build, pr->name, floor, top, native, *sonicSpeed);
}

static int open_build(int b)
{
    if (engines[b])
        return 0;
    engines[b] = bst_open(builds[b].build);
    if (!engines[b])
        return -1;
    engineLevel[b] = bst_get(engines[b], "level");
    engineVoice[b] = bst_get(engines[b], "voice");
    if (strictConv[b] == (iconv_t)-1) {
        char translit[64];

        strictConv[b] = iconv_open(builds[b].codeset, "UTF-8");
        snprintf(translit, sizeof translit, "%s//TRANSLIT", builds[b].codeset);
        translitConv[b] = iconv_open(translit, "UTF-8");
    }
    return 0;
}

static int queue_message(int b, const char *data, SPDMessageType type)
{
    int suppress = type == SPD_MSGTYPE_TEXT
                && settings.punctuation == SPD_PUNCT_NONE;
    int spell = settings.spelling == SPD_SPELL_ON;
    int result;
    char *prepared = NULL;
    Points p = { 0 };

    switch (type) {
    case SPD_MSGTYPE_SOUND_ICON:
        return add_icon(data, strlen(data));
    case SPD_MSGTYPE_KEY:
        prepared = key_text(b, data);
        if (!prepared)
            return -1;
        result = queue_text(b, prepared, 0, 0);
        free(prepared);
        return result;
    case SPD_MSGTYPE_CHAR:
        if (!strcmp(data, " ")) {
            prepared = key_text(b, data);
            result = prepared ? queue_text(b, prepared, 0, 0) : -1;
            free(prepared);
            return result;
        }
        return queue_text(b, data, 0, 1);
    default:
        break;
    }
    result = contains_markup(data) ? parse_ssml(&p, data)
                                   : decode_utf8(&p, data, strlen(data));
    if (result == 0)
        result = respell(b, &p, spell);
    if (result == 0)
        result = queue_points(b, &p, suppress, spell);
    free(p.v);
    return result;
}

/* ---- synthesis -------------------------------------------------------------- */

static int16_t *pcm;
static long pcmCapacity;

static int ensure(int16_t **buffer, long *capacity, long samples)
{
    if (samples <= *capacity)
        return 0;
    int16_t *grown = realloc(*buffer, (size_t)samples * sizeof *grown);
    if (!grown)
        return -1;
    *buffer = grown;
    *capacity = samples;
    return 0;
}

/* Says one piece of engine text into *out from position at, and returns how
 * many samples. The first guess at the room it needs is generous; a piece
 * that fills it is measured and said again. */
static long synthesize_into(bst *h, const char *text, int16_t **out,
                            long *capacity, long at)
{
    long guess = ((long)strlen(text) + 8) * bst_rate(h) / 4;
    long n;

    if (bst_get(h, "rate") > 0)
        guess = guess * (bst_get(h, "rate") + 100) / 100;
    if (ensure(out, capacity, at + guess) != 0)
        return -1;
    n = bst_say(h, text, *out + at, guess);
    if (n == guess) {
        long need = bst_length(h, text);
        if (need < 0 || ensure(out, capacity, at + need) != 0)
            return -1;
        n = bst_say(h, text, *out + at, need);
    }
    return n;
}

static long synthesize(bst *h, const char *text)
{
    return synthesize_into(h, text, &pcm, &pcmCapacity, 0);
}

/* Silence in every build is exactly nought; this is far below anything
 * that is speech. */
enum { QUIET = 8 };

static long speech_start(const int16_t *s, long n)
{
    long i = 0;

    while (i < n && abs(s[i]) <= QUIET)
        i++;
    return i;
}

static long speech_end(const int16_t *s, long n)
{
    long i = n;

    while (i > 0 && abs(s[i - 1]) <= QUIET)
        i--;
    return i;
}

static int has_words(const char *text)
{
    for (; *text; text++)
        if (isalnum((unsigned char)*text) || (unsigned char)*text >= 0x80)
            return 1;
    return 0;
}

/* Says a piece, and never answers a piece with words in it by silence.
 *
 * The early builds read some characters of the Windows set as nothing at all
 * and say the whole sentence as silence; such a sentence is said again with
 * everything above 0x7f taken out. Whatever else silences a sentence, it is
 * said a word at a time as a last resort, so that it is at least heard. */
static long synthesize_safely(bst *h, char *text)
{
    long n = synthesize(h, text);

    if (n < 0 || speech_end(pcm, n) > speech_start(pcm, n) || !has_words(text))
        return n;

    int high = 0;
    for (char *c = text; *c; c++)
        if ((unsigned char)*c >= 0x80)
            high = 1;
    if (high) {
        char *plain = copy_string(text);
        if (plain) {
            for (char *c = plain; *c; c++)
                if ((unsigned char)*c >= 0x80)
                    *c = ' ';
            n = synthesize(h, plain);
            free(plain);
            debug_log("silent sentence, retried without its high bytes");
            if (n < 0 || speech_end(pcm, n) > speech_start(pcm, n))
                return n;
        }
    }

    /* A word at a time, each trimmed, with a short gap between them. */
    static int16_t *words;
    static long wordsCapacity;
    long total = 0, gap = bst_rate(h) / 25;
    char *copy = copy_string(text), *save = NULL;

    debug_log("silent sentence, said a word at a time: %s", text);
    if (!copy)
        return 0;
    for (char *word = strtok_r(copy, " ", &save); word;
         word = strtok_r(NULL, " ", &save)) {
        long got = synthesize_into(h, word, &words, &wordsCapacity, total);
        if (got <= 0)
            continue;
        long a = speech_start(words + total, got);
        long z = speech_end(words + total, got);
        if (z <= a)
            continue;
        memmove(words + total, words + total + a, (size_t)(z - a) * sizeof *words);
        total += z - a;
        if (ensure(&words, &wordsCapacity, total + gap) != 0)
            break;
        memset(words + total, 0, (size_t)gap * sizeof *words);
        total += gap;
    }
    free(copy);
    if (ensure(&pcm, &pcmCapacity, total) != 0)
        return 0;
    if (total)
        memcpy(pcm, words, (size_t)total * sizeof *pcm);
    return total;
}

/* Speech Dispatcher's volume 100, its default, is the build's level brought
 * into line with the others; 0 is half that and -100 silence, as with
 * espeak-ng. Above 24000 the samples are bent softly towards full scale
 * rather than clipped. */
static void apply_gain(int16_t *samples, long n, double gain)
{
    const double knee = 24000.0, room = 32767.0 - knee;

    if (gain == 1.0)
        return;
    for (long i = 0; i < n; i++) {
        double v = samples[i] * gain, a = fabs(v);

        if (a > knee)
            a = knee + room * tanh((a - knee) / room);
        samples[i] = (int16_t)lround(v < 0 ? -a : a);
    }
}

static double output_gain(int b)
{
    double user = settings.volume >= 0 ? 0.5 + 0.5 * settings.volume / 100.0
                                       : 0.5 * (settings.volume + 100) / 100.0;
    return builds[b].gain * user;
}

/* ---- output ----------------------------------------------------------------- */

typedef struct {
    long position;  /* in the samples handed over, where the mark falls */
    const char *name;
} PlacedMark;

static int paused;

/* Reports a mark; a pause asked for stops at the first of Speech
 * Dispatcher's own marks. */
static void report_mark(const char *name)
{
    module_report_index_mark(name);
    if (pauseRequested && !strncmp(name, "__spd_", 6))
        paused = 1;
}

/* Hands samples to the server a fortieth of a second at a time, putting
 * each mark in where its word begins and reading the protocol between
 * blocks so that a stop is heard at once. */
static void output(int rate, int16_t *samples, long n, PlacedMark *marks,
                   size_t markCount)
{
    const uint16_t probe = 1;
    long block = rate / 40;
    size_t next = 0;

    for (long at = 0; at < n && !stopRequested && !paused; at += block) {
        while (next < markCount && marks[next].position <= at && !paused)
            report_mark(marks[next++].name);
        if (paused)
            break;

        AudioTrack track = {
            .bits = 16,
            .num_channels = 1,
            .sample_rate = rate,
            .num_samples = (int)(n - at < block ? n - at : block),
            .samples = samples + at,
        };

        module_tts_output_server(&track, *(const unsigned char *)&probe
                                         ? SPD_AUDIO_LE : SPD_AUDIO_BE);
        module_process(STDIN_FILENO, 0);
    }
    while (next < markCount && !stopRequested && !paused)
        report_mark(marks[next++].name);
}

/* Says one piece. The silence before the speech is cut away, and the silence
 * after it is cut down: to almost nothing at the end of the message, and to
 * a short pause, shorter as the speech is faster, between sentences. Marks
 * are placed in proportion to where they stand in the text: the engine says
 * nothing about where a word falls in its audio, and a mark only has to
 * come near its word. */
static void speak_piece(int b, Item *item, int last, double sonicSpeed,
                        double speed)
{
    bst *h = engines[b];
    int rate = bst_rate(h);
    PlacedMark *placed = NULL;
    long n = 0, start = 0, end = 0, length;

    if (item->text) {
        n = synthesize_safely(h, item->text);
        if (n < 0)
            n = 0;
    }
    if (rawOutput) {
        start = 0;
        end = length = n;
    } else {
        double pause = last ? 0.02
                     : item->ending == END_SENTENCE ? 0.22 : 0.06;
        long keep = (long)(pause / speed * rate);

        start = speech_start(pcm, n);
        end = speech_end(pcm, n);
        if (end < start)
            end = start;
        length = end + (n - end < keep ? n - end : keep) - start;
        apply_gain(pcm + start, length, output_gain(b));
    }

    int16_t *samples = pcm + start;
    int16_t *sped = NULL;
    double scale = 1.0;

    if (sonicSpeed != 1.0 && length > 0) {
        sonicStream stream = sonicCreateStream(rate, 1);

        if (stream) {
            sonicSetSpeed(stream, (float)sonicSpeed);
            sonicSetQuality(stream, 1);
            sonicWriteShortToStream(stream, samples, (int)length);
            sonicFlushStream(stream);
            int available = sonicSamplesAvailable(stream);
            if (available > 0 && (sped = malloc((size_t)available * sizeof *sped))) {
                int got = sonicReadShortFromStream(stream, sped, available);
                scale = (double)got / (double)length;
                samples = sped;
                length = got;
            }
            sonicDestroyStream(stream);
        }
    }

    if (item->markCount && (placed = malloc(item->markCount * sizeof *placed))) {
        double spoken = (double)(end - start) * scale;

        for (size_t k = 0; k < item->markCount; k++) {
            double share = item->length
                         ? (double)item->marks[k].offset / (double)item->length
                         : 1.0;
            placed[k].position = (long)(share * spoken);
            placed[k].name = item->marks[k].name;
        }
    }
    output(rate, samples, length, placed, placed ? item->markCount : 0);
    free(placed);
    free(sped);
}

/* ---- the module interface ------------------------------------------------- */

static void free_voice_list(void)
{
    if (!voiceList)
        return;
    for (size_t i = 0; voiceList[i]; i++) {
        free(voiceList[i]->name);
        free(voiceList[i]->language);
        free(voiceList[i]->variant);
        free(voiceList[i]);
    }
    free(voiceList);
    voiceList = NULL;
}

static int build_voice_list(void)
{
    size_t at = 0;

    voiceList = calloc(BUILD_COUNT * PRESET_COUNT + 1, sizeof *voiceList);
    if (!voiceList)
        return -1;
    for (int b = 0; b < BUILD_COUNT; b++) {
        for (int p = 0; p < PRESET_COUNT; p++) {
            SPDVoice *v = calloc(1, sizeof *v);
            size_t n = strlen(builds[b].tag) + strlen(presets[p].name) + 2;

            if (!v)
                return -1;
            voiceList[at++] = v;
            v->name = malloc(n);
            v->language = copy_string(builds[b].language);
            v->variant = copy_string(presets[p].name);
            if (!v->name || !v->language || !v->variant)
                return -1;
            snprintf(v->name, n, "%s-%s", builds[b].tag, presets[p].name);
        }
    }
    return 0;
}

int module_config(const char *configFile)
{
    FILE *file;
    char line[512];

    if (!configFile || !*configFile)
        return 0;
    file = fopen(configFile, "r");
    if (!file)
        return 0;
    while (fgets(line, sizeof line, file)) {
        char key[64], a[64], z[64];
        int n = sscanf(line, " %63s %63s %63s", key, a, z);

        if (n < 1 || key[0] == '#')
            continue;
        if (!strcasecmp(key, "DefaultBuild") && n == 3) {
            int b = build_index(z);
            if (b < 0 || strncasecmp(builds[b].language, a, 2)) {
                fprintf(stderr, "bestspeech: ignoring DefaultBuild %s %s\n", a, z);
                continue;
            }
            defaultBuild[!strncasecmp(a, "fr", 2) ? 0 : 1] = b;
        } else if (!strcasecmp(key, "MaxSpeed") && n >= 2) {
            double s = atof(a);
            if (s >= 1.0 && s <= 8.0)
                maxSpeed = s;
        } else if (!strcasecmp(key, "RawOutput") && n >= 2) {
            rawOutput = atoi(a) != 0;
        } else if (!strcasecmp(key, "Debug") && n >= 2) {
            debugEnabled = atoi(a) != 0;
        } else {
            fprintf(stderr, "bestspeech: ignoring unknown setting %s\n", key);
        }
    }
    fclose(file);
    return 0;
}

int module_init(char **statusInfo)
{
    module_audio_set_server();
    for (int b = 0; b < BUILD_COUNT; b++)
        strictConv[b] = translitConv[b] = (iconv_t)-1;
    /* Every build is opened now, so that none costs anything the first time
       it is asked for. */
    for (int b = 0; b < BUILD_COUNT; b++)
        if (open_build(b) != 0) {
            *statusInfo = copy_string("BestSpeech could not open its engine");
            return -1;
        }
    if (build_voice_list() != 0) {
        *statusInfo = copy_string("BestSpeech could not build its voice list");
        return -1;
    }
    *statusInfo = copy_string("BestSpeech initialized");
    debug_log("initialized");
    return 0;
}

SPDVoice **module_list_voices(void)
{
    return voiceList;
}

void module_speak_sync(const char *data, size_t bytes, SPDMessageType type)
{
    char *message;
    int b, p;
    double sonicSpeed, speed;
    size_t lastAudio = 0;

    stopRequested = 0;
    pauseRequested = 0;
    paused = 0;
    clear_items();
    clear_mark_names();
    resolve_voice(&b, &p);
    if (bytes == SIZE_MAX || open_build(b) != 0
        || !(message = copy_bytes(data, bytes))) {
        module_speak_error();
        return;
    }
    debug_log("speak type %d: %s", (int)type, message);
    if (queue_message(b, message, type) != 0) {
        free(message);
        clear_items();
        module_speak_error();
        return;
    }
    free(message);
    configure(b, p, &sonicSpeed, &speed);
    for (size_t i = 0; i < itemCount; i++)
        if (items[i].kind == ITEM_TEXT && items[i].text)
            lastAudio = i;

    module_speak_ok();
    module_report_event_begin();
    for (size_t i = 0; i < itemCount && !stopRequested && !paused; i++) {
        if (items[i].kind == ITEM_ICON)
            module_report_icon(items[i].text);
        else
            speak_piece(b, &items[i], i >= lastAudio, sonicSpeed, speed);
        module_process(STDIN_FILENO, 0);
    }
    clear_items();
    clear_mark_names();
    if (paused)
        module_report_event_pause();
    else if (stopRequested)
        module_report_event_stop();
    else
        module_report_event_end();
}

int module_stop(void)
{
    stopRequested = 1;
    return 0;
}

size_t module_pause(void)
{
    pauseRequested = 1;
    return 0;
}

int module_close(void)
{
    for (int b = 0; b < BUILD_COUNT; b++) {
        bst_close(engines[b]);
        engines[b] = NULL;
        if (strictConv[b] != (iconv_t)-1)
            iconv_close(strictConv[b]);
        if (translitConv[b] != (iconv_t)-1)
            iconv_close(translitConv[b]);
        strictConv[b] = translitConv[b] = (iconv_t)-1;
    }
    clear_items();
    free(items);
    items = NULL;
    itemCapacity = 0;
    clear_mark_names();
    free(markNames);
    markNames = NULL;
    markNameCapacity = 0;
    free(pcm);
    pcm = NULL;
    pcmCapacity = 0;
    free_voice_list();
    free(settings.language);
    settings.language = NULL;
    free(settings.synthesisVoice);
    settings.synthesisVoice = NULL;
    if (debugFile) {
        fclose(debugFile);
        debugFile = NULL;
    }
    return 0;
}

int module_set(const char *variable, const char *value)
{
    if (!strcmp(variable, "rate"))
        return parse_number(value, -100, 100, &settings.rate);
    if (!strcmp(variable, "pitch"))
        return parse_number(value, -100, 100, &settings.pitch);
    if (!strcmp(variable, "pitch_range"))
        return parse_number(value, -100, 100, &settings.pitchRange);
    if (!strcmp(variable, "volume"))
        return parse_number(value, -100, 100, &settings.volume);
    if (!strcmp(variable, "punctuation_mode")) {
        if (!strcmp(value, "all")) settings.punctuation = SPD_PUNCT_ALL;
        else if (!strcmp(value, "none")) settings.punctuation = SPD_PUNCT_NONE;
        else if (!strcmp(value, "some")) settings.punctuation = SPD_PUNCT_SOME;
        else if (!strcmp(value, "most")) settings.punctuation = SPD_PUNCT_MOST;
        else return -1;
        return 0;
    }
    if (!strcmp(variable, "spelling_mode")) {
        if (!strcmp(value, "on")) settings.spelling = SPD_SPELL_ON;
        else if (!strcmp(value, "off")) settings.spelling = SPD_SPELL_OFF;
        else return -1;
        return 0;
    }
    if (!strcmp(variable, "cap_let_recogn")) {
        if (!strcmp(value, "none")) settings.capitals = SPD_CAP_NONE;
        else if (!strcmp(value, "spell")) settings.capitals = SPD_CAP_SPELL;
        else if (!strcmp(value, "icon")) settings.capitals = SPD_CAP_ICON;
        else return -1;
        return 0;
    }
    if (!strcmp(variable, "voice")) {
        static const char *const names[] = {
            "male1", "male2", "male3", "female1", "female2", "female3",
            "child_male", "child_female"
        };

        for (int i = 0; i < 8; i++)
            if (!strcmp(value, names[i])) {
                settings.voiceType = (SPDVoiceType)(i + 1);
                return 0;
            }
        return -1;
    }
    if (!strcmp(variable, "language")) {
        free(settings.language);
        settings.language = copy_string(value);
        return 0;
    }
    if (!strcmp(variable, "synthesis_voice")) {
        free(settings.synthesisVoice);
        settings.synthesisVoice = copy_string(value);
        return 0;
    }
    return -1;
}

int module_audio_set(const char *variable, const char *value)
{
    return !strcmp(variable, "audio_output_method")
        && !strcmp(value, "server") ? 0 : -1;
}

int module_audio_init(char **statusInfo)
{
    (void)statusInfo;
    return 0;
}

int module_loglevel_set(const char *variable, const char *value)
{
    if (strcmp(variable, "log_level"))
        return -1;
    return parse_number(value, 0, 5, &logLevel);
}

int module_debug(int enable, const char *filename)
{
    FILE *file = NULL;

    if (enable) {
        if (!filename)
            return -1;
        file = fopen(filename, "w");
        if (!file)
            return -1;
    }
    if (debugFile)
        fclose(debugFile);
    debugFile = file;
    debugEnabled = enable;
    return 0;
}

int module_loop(void)
{
    return module_process(STDIN_FILENO, 1);
}
