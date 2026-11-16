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
 */

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

#include <sonic.h>

#include "bst.h"

/* ---- the builds and the presets ---------------------------------------- */

typedef struct {
    const char *build;    /* openbst's name for it */
    const char *tag;      /* the prefix of the voice names */
    const char *language; /* what Speech Dispatcher is told */
    const char *codeset;  /* what the build reads its text in */
    int commaEndsText;    /* says nothing after the first comma */
} BuildInfo;

/* The 2006 builds fold the Windows Western set into their own encoding. The
 * 1995 and 1998 ones read the DOS set: a Windows byte above 0x7f makes them
 * say nothing at all.
 *
 * Handed a whole text in one call, the 2006 builds stop at its first comma:
 * "Bonjour, au revoir." comes out as "Bonjour". Their host fed them a phrase
 * at a time, so the module does the same and ends a piece at every comma. */
static const BuildInfo builds[] = {
    { "2006FRE", "fr-2006", "fr", "CP1252", 1 },
    { "1998FRN", "fr-1998", "fr", "CP850",  0 },
    { "1995",    "en-1995", "en", "CP850",  0 },
    { "1998ENG", "en-1998", "en", "CP850",  0 },
    { "2006ENG", "en-2006", "en", "CP1252", 1 },
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

/* ---- the queue one message becomes --------------------------------------- */

enum { ITEM_TEXT, ITEM_MARK, ITEM_ICON };

typedef struct {
    int kind;
    char *value; /* engine text, mark name or icon name */
} Item;

static Item *items;
static size_t itemCount, itemCapacity;

static void clear_items(void)
{
    for (size_t i = 0; i < itemCount; i++)
        free(items[i].value);
    itemCount = 0;
}

static int add_item(int kind, const char *value, size_t length)
{
    char *copy;

    if (itemCount == itemCapacity) {
        size_t capacity = itemCapacity ? itemCapacity * 2 : 16;
        Item *grown = realloc(items, capacity * sizeof *items);

        if (!grown)
            return -1;
        items = grown;
        itemCapacity = capacity;
    }
    copy = malloc(length + 1);
    if (!copy)
        return -1;
    memcpy(copy, value, length);
    copy[length] = 0;
    items[itemCount].kind = kind;
    items[itemCount].value = copy;
    itemCount++;
    return 0;
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
 * its marks. A tilde is dropped: it is the engine's own command lead-in, and
 * a tilde and bar in the text would end the phrase. */
static uint32_t normalize(uint32_t c, int *expandEllipsis)
{
    *expandEllipsis = 0;
    switch (c) {
    case 0x2026: *expandEllipsis = 1; return '.';
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

static const char *word_for(int b, const char *english, const char *french)
{
    return !strcmp(builds[b].language, "fr") ? french : english;
}

/* Ends the piece being built, unless there is nothing in it but spaces:
 * the 2006 builds say an empty text as most of a second of silence. */
static int flush_piece(Buffer *out)
{
    size_t i;
    int result = 0;

    for (i = 0; i < out->length && out->data[i] == ' '; i++)
        ;
    if (i < out->length)
        result = add_item(ITEM_TEXT, out->data, out->length);
    out->length = 0;
    if (out->data)
        out->data[0] = 0;
    return result;
}

/* Turns UTF-8 text into engine text items, one to a sentence so that a long
 * text starts speaking as soon as its first sentence is ready and a stop
 * never waits on more than one. */
static int queue_text(int b, const char *text, int suppressPunctuation,
                      int spell)
{
    const unsigned char *at = (const unsigned char *)text;
    Buffer out = { 0 };
    uint32_t previous = ' ', beforeSpace = ' ';
    size_t sinceBreak = 0;
    int result = 0;

    buffer_append(&out, "", 0);
    while (*at && result == 0) {
        uint32_t raw = utf8_next(&at);
        int ellipsis;
        uint32_t c = normalize(raw, &ellipsis);
        uint32_t next;
        const unsigned char *peek = at;

        if (!c)
            continue;
        next = *peek ? normalize(utf8_next(&peek), &(int){ 0 }) : ' ';

        if (suppressPunctuation && !spell && is_droppable_symbol(c)
            && !(c == '\'' && is_letter_or_digit(previous)
                 && is_letter_or_digit(next)))
            c = ' ';

        int commaWritten = 0;

        if (spell && c != ' ') {
            if (settings.capitals != SPD_CAP_NONE && is_upper(c)) {
                if (settings.capitals == SPD_CAP_SPELL) {
                    buffer_append_str(&out, " ");
                    buffer_append_str(&out, word_for(b, "capital", "majuscule"));
                    buffer_append_str(&out, " ");
                } else {
                    if (flush_piece(&out))
                        result = -1;
                    if (add_item(ITEM_ICON, "capital", 7))
                        result = -1;
                }
            }
            encode_char(b, c, &out);
            buffer_append(&out, " ", 1);
            commaWritten = c == ',';
        } else if (ellipsis) {
            buffer_append(&out, "...", 3);
        } else if (c == ',' && builds[b].commaEndsText
                   && (is_digit(previous)
                       || (previous == ' ' && is_digit(beforeSpace)))
                   && is_digit(next)) {
            /* A comma between digits would cut the number off too: in French
               it is the decimal mark, in English the thousands separator.
               Speech Dispatcher's symbol pass writes 1,5 as "1 ,5". */
            if (!strcmp(builds[b].language, "fr"))
                buffer_append_str(&out, " virgule ");
        } else {
            encode_char(b, c, &out);
            commaWritten = c == ',';
        }
        sinceBreak++;

        /* A sentence ends at its mark when a space follows, and in the 2006
           builds a phrase ends at any comma; a run with no mark at all is
           broken at a space once it is long. */
        if (((c == '.' || c == '?' || c == '!' || c == ';' || c == ':')
             && next == ' ' && !spell)
            || (commaWritten && builds[b].commaEndsText)
            || (c == ' ' && sinceBreak > 400)) {
            if (flush_piece(&out))
                result = -1;
            sinceBreak = 0;
        }
        if (c != ' ')
            beforeSpace = c;
        previous = c;
    }
    if (result == 0 && flush_piece(&out))
        result = -1;
    free(out.data);
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

static int append_entity(Buffer *text, const char *entity, size_t length)
{
    static const struct { const char *name, *value; } entities[] = {
        { "amp", "&" }, { "lt", "<" }, { "gt", ">" },
        { "quot", "\"" }, { "apos", "'" }, { "nbsp", " " },
    };

    if (length > 1 && entity[0] == '#') {
        long c = entity[1] == 'x' || entity[1] == 'X'
               ? strtol(entity + 2, NULL, 16) : strtol(entity + 1, NULL, 10);
        if (c > 0 && c < 0x110000) {
            utf8_put(text, (uint32_t)c);
            return 0;
        }
        return buffer_append(text, " ", 1);
    }
    for (size_t i = 0; i < sizeof entities / sizeof entities[0]; i++)
        if (strlen(entities[i].name) == length
            && !strncmp(entity, entities[i].name, length))
            return buffer_append_str(text, entities[i].value);
    return buffer_append(text, " ", 1);
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

static int queue_ssml(int b, const char *data, int suppressPunctuation,
                      int spell)
{
    Buffer text = { 0 };
    const char *at = data;
    int result = 0;

    buffer_append(&text, "", 0);
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
                if (name) {
                    result = queue_text(b, text.data, suppressPunctuation, spell);
                    text.length = 0;
                    text.data[0] = 0;
                    if (result == 0)
                        result = add_item(ITEM_MARK, name, nameLength);
                }
            } else if (tag_matches(at, length, "break")) {
                result = buffer_append(&text, ". ", 2);
            }
            at = end + 1;
        } else if (*at == '&') {
            const char *end = strchr(at + 1, ';');

            if (end && end - at < 12) {
                result = append_entity(&text, at + 1, (size_t)(end - at - 1));
                at = end + 1;
            } else {
                result = buffer_append(&text, at++, 1);
            }
        } else {
            const char *end = strpbrk(at, "<&");
            size_t length = end ? (size_t)(end - at) : strlen(at);

            result = buffer_append(&text, at, length);
            at += length;
        }
    }
    if (result == 0 && text.length)
        result = queue_text(b, text.data, suppressPunctuation, spell);
    free(text.data);
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
 * Sonic, as BstSpeech-sapi did. */
static void rate_to_engine(int rate, int *native, double *sonic)
{
    double speed = rate >= 0 ? pow(maxSpeed, rate / 100.0)
                             : pow(2.0, rate / 50.0);

    *sonic = 1.0;
    if (rate == 0) {
        *native = 0;
        return;
    }
    if (speed <= rateTable[0].speed) {
        *native = rateTable[0].native;
        *sonic = speed / rateTable[0].speed;
        return;
    }
    if (speed >= rateTable[RATE_POINTS - 1].speed) {
        *native = rateTable[RATE_POINTS - 1].native;
        *sonic = speed / rateTable[RATE_POINTS - 1].speed;
        return;
    }
    for (int i = 1; i < RATE_POINTS; i++) {
        if (speed <= rateTable[i].speed) {
            double a = log(rateTable[i - 1].speed), z = log(rateTable[i].speed);
            double t = (log(speed) - a) / (z - a);
            *native = (int)lround(rateTable[i - 1].native
                                  + t * (rateTable[i].native - rateTable[i - 1].native));
            return;
        }
    }
}

static int clamp(int value, int low, int high)
{
    return value < low ? low : value > high ? high : value;
}

/* Puts the preset and the relative settings on the build's handle. */
static void configure(int b, int p, double *sonicSpeed)
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
    rate_to_engine(settings.rate, &native, sonicSpeed);
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

    switch (type) {
    case SPD_MSGTYPE_SOUND_ICON:
        return add_item(ITEM_ICON, data, strlen(data));
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
    if (contains_markup(data))
        return queue_ssml(b, data, suppress, spell);
    return queue_text(b, data, suppress, spell);
}

/* ---- synthesis and output ----------------------------------------------- */

static int16_t *pcm;
static long pcmCapacity;

static int ensure_pcm(long samples)
{
    if (samples <= pcmCapacity)
        return 0;
    int16_t *grown = realloc(pcm, (size_t)samples * sizeof *pcm);
    if (!grown)
        return -1;
    pcm = grown;
    pcmCapacity = samples;
    return 0;
}

/* Says one piece of engine text into pcm and returns how many samples. The
 * first guess at the room it needs is generous; a piece that fills it is
 * measured and said again. */
static long synthesize(bst *h, const char *text)
{
    long guess = ((long)strlen(text) + 8) * bst_rate(h) / 4;
    long n;

    if (bst_get(h, "rate") > 0)
        guess = guess * (bst_get(h, "rate") + 100) / 100;
    if (ensure_pcm(guess) != 0)
        return -1;
    n = bst_say(h, text, pcm, guess);
    if (n == guess) {
        long need = bst_length(h, text);
        if (need < 0 || ensure_pcm(need) != 0)
            return -1;
        n = bst_say(h, text, pcm, need);
    }
    return n;
}

/* The early builds read some characters of the Windows set as nothing at all
 * and say the whole sentence as silence. Should a sentence come back empty,
 * it is said again with everything above 0x7f taken out. */
static long synthesize_safely(bst *h, char *text)
{
    long n = synthesize(h, text);
    int high = 0;

    for (char *c = text; *c; c++)
        if ((unsigned char)*c >= 0x80)
            high = 1;
    if (n == 0 && high) {
        for (char *c = text; *c; c++)
            if ((unsigned char)*c >= 0x80)
                *c = ' ';
        debug_log("silent sentence, retried without its high bytes");
        n = synthesize(h, text);
    }
    return n;
}

static double volume_gain(void)
{
    return settings.volume <= 0 ? (settings.volume + 100) / 100.0
                                : 1.0 + settings.volume / 100.0;
}

static void apply_gain(int16_t *samples, long n, double gain)
{
    if (gain == 1.0)
        return;
    for (long i = 0; i < n; i++) {
        long v = lround(samples[i] * gain);
        samples[i] = (int16_t)(v > 32767 ? 32767 : v < -32768 ? -32768 : v);
    }
}

/* Hands samples to the server a twentieth of a second at a time, reading
 * the protocol between them so that a stop is heard at once. */
static void output(int rate, int16_t *samples, long n)
{
    const uint16_t probe = 1;
    long block = rate / 20;

    for (long at = 0; at < n && !stopRequested; at += block) {
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
}

static void speak_piece(int b, char *text, double sonicSpeed)
{
    bst *h = engines[b];
    int rate = bst_rate(h);
    long n = synthesize_safely(h, text);

    if (n <= 0)
        return;
    apply_gain(pcm, n, volume_gain());
    if (sonicSpeed == 1.0) {
        output(rate, pcm, n);
        return;
    }

    sonicStream stream = sonicCreateStream(rate, 1);
    if (!stream) {
        output(rate, pcm, n);
        return;
    }
    sonicSetSpeed(stream, (float)sonicSpeed);
    sonicSetQuality(stream, 1);
    sonicWriteShortToStream(stream, pcm, (int)n);
    sonicFlushStream(stream);
    int available = sonicSamplesAvailable(stream);
    if (available > 0) {
        int16_t *out = malloc((size_t)available * sizeof *out);
        if (out) {
            int got = sonicReadShortFromStream(stream, out, available);
            if (got > 0)
                output(rate, out, got);
            free(out);
        }
    }
    sonicDestroyStream(stream);
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
    if (open_build(defaultBuild[0]) != 0 || open_build(defaultBuild[1]) != 0) {
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
    int b, p, paused = 0;
    double sonicSpeed;

    stopRequested = 0;
    pauseRequested = 0;
    clear_items();
    resolve_voice(&b, &p);
    if (bytes == SIZE_MAX || open_build(b) != 0
        || !(message = malloc(bytes + 1))) {
        module_speak_error();
        return;
    }
    memcpy(message, data, bytes);
    message[bytes] = 0;
    debug_log("speak type %d: %s", (int)type, message);
    if (queue_message(b, message, type) != 0) {
        free(message);
        module_speak_error();
        return;
    }
    free(message);
    configure(b, p, &sonicSpeed);

    module_speak_ok();
    module_report_event_begin();
    for (size_t i = 0; i < itemCount && !stopRequested; i++) {
        switch (items[i].kind) {
        case ITEM_TEXT:
            speak_piece(b, items[i].value, sonicSpeed);
            break;
        case ITEM_MARK:
            module_report_index_mark(items[i].value);
            if (pauseRequested && !strncmp(items[i].value, "__spd_", 6))
                paused = 1;
            break;
        case ITEM_ICON:
            module_report_icon(items[i].value);
            break;
        }
        module_process(STDIN_FILENO, 0);
        if (paused)
            break;
    }
    clear_items();
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
