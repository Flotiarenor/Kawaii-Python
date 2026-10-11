/*
 * Message replacement table for CPython 3.11+ (developed against 3.15).
 *
 * Ported from a 3.10 fork that replaced exception messages at print time.  Two
 * upstream changes shape this version:
 *
 *   1. 3.11 replaced the type/value/traceback triple with a single exception
 *      instance, so the hook no longer rebuilds a value tuple.
 *   2. Not every exception derives its displayed text from args[0].  OSError
 *      and UnicodeDecodeError build the message from dedicated fields and
 *      KeyError applies repr().  Those instances are now matched on the text
 *      they actually display, which is the reason the table understands
 *      patterns at all.  Writing the replacement into args[0] cannot reach
 *      such a display, so it is carried by a subclass whose __str__ returns
 *      it instead: see the display proxy section below.
 *
 * Everything is plain C on public API: nothing here imports Python code, so a
 * traceback stays printable no matter how broken the interpreter's import
 * machinery happens to be at that moment.  The pattern engine in the middle of
 * this file does not use the Python API at all, so it can be compiled and
 * exercised on its own (see MESSAGE_TABLE_PATTERN_TEST below).
 *
 * Table format (UTF-8, one entry per line, blank lines and '#' comments
 * ignored).  The two halves of an entry are separated by a TAB:
 *
 *     # original message<TAB>hex-encoded UTF-8 replacement
 *     division by zero<TAB>e7aca8e89b8be6898de4bc9ae68a8a30e694bee588b0e999a4e695b0e590a721
 *
 * The key is the message as it is, so it may hold commas, spaces, quotes and
 * parentheses without escaping.  The TAB is the only separator and cannot be
 * part of a key, and neither can a newline, which ends the line.  The
 * right-hand side is what Python's bytes.hex() produces:
 *
 *     print(input().encode('utf-8').hex())
 *
 * A key without '*' is a literal message: the display text has to be equal to
 * it and the replacement is used as it is.  A key with at least one '*' is a
 * pattern: '*' stands for any run of characters, possibly empty, the runs are
 * numbered in order of appearance, and the replacement can refer to the n-th
 * of them as "%n".  Patterns are matched against the text the exception
 * displays, so a single entry such as
 *
 *     unsupported operand type(s) for *: '*' and '*'<TAB>...
 *
 * covers every pair of operand types, where a literal key can only cover the
 * pairs somebody enumerated in advance.
 *
 * Matching is leftmost and non-greedy: every '*' takes the shortest run that
 * still lets the rest of the pattern match, earlier wildcards being decided
 * first.  '*' is the only special character, so nothing has to be escaped.  A
 * replacement that refers to a capture the pattern does not have ("%2" for a
 * pattern with one '*', or "%0") is refused and the original message is kept,
 * and so is a pattern with more than 64 wildcards.
 *
 * What the reader accepts, and where it looks, is part of the format:
 *
 *   - A line is at most MESSAGE_TABLE_MAX_LINE bytes including the newline.  A
 *     longer line is skipped as a whole and counted, never truncated silently:
 *     half an entry would replace a message with text nobody wrote.
 *   - A line holding a NUL byte is skipped, because everything downstream
 *     treats the line as text and a NUL would cut it in half invisibly.
 *   - Both halves have to be valid UTF-8.  The key is decoded directly, the
 *     value after hex decoding, and an entry whose bytes are not UTF-8 is
 *     dropped whole instead of being replaced with U+FFFD.
 *   - An empty replacement is allowed: an entry that maps a message to nothing
 *     is a way to silence it on purpose, which is different from a malformed
 *     line and is therefore not rejected.
 *   - The table itself is capped as well (MESSAGE_TABLE_MAX_BYTES on disk,
 *     MESSAGE_TABLE_MAX_ENTRIES entries) and has to be a regular file.  The
 *     loader runs while an exception is being displayed, so a table that is a
 *     FIFO, a device or simply enormous must not be able to hang or exhaust the
 *     process from there.
 *   - The lookup walks the candidates in order and takes the first one that
 *     opens: $PYTHON_MESSAGE_TABLE, $PYTHONHOME/lookup-table.txt,
 *     <sys.prefix>/lookup-table.txt and finally ./lookup-table.txt.  A
 *     candidate that cannot be read does not stop the walk, and a table that
 *     appears later in the life of the process is picked up by the next
 *     lookup.  The two environment candidates are skipped under -E and the
 *     current directory under -I; "-X message-table=off" switches the whole
 *     feature off for a run.
 */

#if defined(MESSAGE_TABLE_PATTERN_TEST)
/*
 * Standalone build of the pattern engine, without the interpreter, so that the
 * matcher can be compiled and run with a plain C compiler:
 *
 *     gcc -DMESSAGE_TABLE_PATTERN_TEST -o pattern_test message_table.c
 *     ./pattern_test          # built-in cases
 *     ./pattern_test -        # cases read from stdin, hex-encoded
 *     ./pattern_test --split  # table lines from stdin, split as the loader does
 *
 * Only the pattern engine, the table-line parser and the harness at the end of
 * this file are compiled in; every part that needs Python.h is skipped.
 */
#define MESSAGE_TABLE_NO_PYTHON 1
#endif

#if defined(MESSAGE_TABLE_NO_PYTHON)
#include <stdio.h>
#include <stdlib.h>
#elif defined(MESSAGE_TABLE_STUB_BUILD)
/* Syntax/functional test build: the stub stands in for the CPython headers. */
#include "py_stub_api.h"
#else
#include "Python.h"
#endif

#include <errno.h>
#include <string.h>
#include <sys/stat.h>

#if defined(MS_WINDOWS)
#  include <io.h>                  /* _fileno(), _fstat64() */
#endif

/* ------------------------------------------------------------------ */
/* pattern engine (no Python API)                                      */
/* ------------------------------------------------------------------ */

/*
 * A key containing at least one '*' is a pattern.  Everything else in it is
 * literal text that has to match byte for byte; '*' is the only character the
 * matcher treats specially, so in particular the '%' that introduces a capture
 * reference in the replacement needs no escaping in the key.
 */
#define MESSAGE_PATTERN_WILDCARD '*'
#define MESSAGE_PATTERN_REFERENCE '%'

/*
 * Longest accepted table line, replacement included, newline counted.  A
 * longer line is skipped whole (see the file comment): the loader no longer
 * reads through a fixed-size buffer, so nothing is truncated by accident, and
 * this is a policy limit rather than a buffer size.
 */
#define MESSAGE_TABLE_MAX_LINE 16384

/*
 * Upper bound on the wildcards of one pattern.  A pattern is hand-written text
 * for a message table, so one with more wildcards than this cannot come from a
 * person writing one, and the bound keeps both the recursive matcher's stack
 * use and the caller's capture buffer finite.  Longer patterns are dropped
 * while the table is read, like any other malformed line.
 */
#define MESSAGE_PATTERN_MAX_STARS 64

/*
 * How many positions the matcher may try for one text.  A non-greedy search
 * backtracks, so a pattern such as "*a*a*a*a*b" against a long run of 'a' can
 * cost far more than a table author would expect.  Running out of budget is
 * reported as "no match", which has the same visible result as a pattern that
 * does not match: the original message is printed.
 *
 * This bound is per pattern.  Checking a whole table is bounded as well, by
 * MESSAGE_PATTERN_CALL_BUDGET below, because a table with thousands of
 * pathological patterns would otherwise add up: the per-pattern bound alone
 * lets every one of them spend its own budget, which is what turned a large
 * hand-written table into minutes of work for a single traceback.
 */
#define MESSAGE_PATTERN_BUDGET 1000000

/*
 * How many positions one lookup may try across every pattern of the table.
 * Generated patterns can be worse than hand-written ones, and a table is read
 * from a path an attacker may control, so the total is bounded here: once the
 * shared budget runs out the remaining patterns count as non-matching and the
 * original message is printed.  The value covers a large table of ordinary
 * patterns with room to spare (the shipped table needs a few thousand steps in
 * the worst case) while keeping the tail of a hostile table in the millisecond
 * range.
 */
#define MESSAGE_PATTERN_CALL_BUDGET 10000000

typedef struct {
    const char *text;
    size_t text_size;
    const char *pattern;
    size_t pattern_size;
    size_t star_count;
    size_t *captures;           /* (offset, size) per wildcard, or NULL */
    size_t budget;              /* positions left for this pattern */
    size_t *shared_budget;      /* positions left for the whole lookup, or
                                   NULL when only one pattern is checked */
} PatternSearch;

/*
 * Number of wildcards in a pattern.  Zero means the key is a literal message,
 * which is the one and only rule for telling the two kinds of entry apart.
 */
static size_t
pattern_star_count(const char *pattern, size_t pattern_size)
{
    size_t count = 0;
    for (size_t i = 0; i < pattern_size; i++) {
        if (pattern[i] == MESSAGE_PATTERN_WILDCARD) {
            count++;
        }
    }
    return count;
}

/*
 * The `index`-th literal segment of a pattern: the bytes between the
 * (`index`-1)-th and the `index`-th wildcard.  A pattern with n wildcards has
 * n + 1 segments, and the first and the last of them may be empty.
 */
static void
pattern_segment(const char *pattern, size_t pattern_size, size_t index,
                size_t *start, size_t *size)
{
    size_t position = 0;
    size_t current = 0;
    size_t begin = 0;

    while (position <= pattern_size) {
        if (position == pattern_size
            || pattern[position] == MESSAGE_PATTERN_WILDCARD) {
            if (current == index) {
                *start = begin;
                *size = position - begin;
                return;
            }
            current++;
            begin = position + 1;
        }
        position++;
    }
    /* Unreachable: callers pass an index below the segment count. */
    *start = pattern_size;
    *size = 0;
}

/*
 * Length in bytes of the UTF-8 sequence that starts at text[position].  At
 * least 1, so a caller that walks a text with this can never stand still; a
 * byte that is not a valid lead byte, or a sequence cut short by the end of
 * the text, counts as a single character.  Both sides of a match are expected
 * to be valid UTF-8 (the table is decoded as such when it is read, and the
 * display text of an exception always is), and this fallback only keeps the
 * matcher total when one of them is not.
 */
static size_t
pattern_char_size(const char *text, size_t text_size, size_t position)
{
    unsigned char lead = (unsigned char)text[position];
    size_t size;

    if (lead < 0x80) {
        return 1;
    }
    if ((lead & 0xE0) == 0xC0) {
        size = 2;
    }
    else if ((lead & 0xF0) == 0xE0) {
        size = 3;
    }
    else if ((lead & 0xF8) == 0xF0) {
        size = 4;
    }
    else {
        return 1;               /* a continuation byte or an invalid lead */
    }
    if (position + size > text_size) {
        return 1;           /* the sequence runs past the end of the text */
    }
    for (size_t i = 1; i < size; i++) {
        if (((unsigned char)text[position + i] & 0xC0) != 0x80) {
            return 1;           /* not a continuation byte: one byte it is */
        }
    }
    return size;
}

/*
 * Match the segments from `index` on against the text from `position` on, and
 * record the run each wildcard takes on the way out.
 *
 * Wildcards are decided left to right and each one takes the shortest run that
 * lets the remaining segments match, which is the non-greedy rule of the table
 * format: the positions after a segment are tried in increasing order, so the
 * first success is the shortest run.
 */
static int
pattern_search(PatternSearch *search, size_t index, size_t position)
{
    size_t start, size;
    pattern_segment(search->pattern, search->pattern_size, index,
                    &start, &size);

    if (size > search->text_size - position) {
        return 0;
    }
    if (size > 0
        && memcmp(search->text + position, search->pattern + start,
                  size) != 0) {
        return 0;
    }
    size_t after = position + size;

    if (index == search->star_count) {
        /* The last segment has to end the text: a pattern describes the whole
           message, not a part of it. */
        return after == search->text_size;
    }
    if (search->budget == 0
        || (search->shared_budget != NULL && *search->shared_budget == 0)) {
        return 0;
    }
    search->budget--;
    if (search->shared_budget != NULL) {
        (*search->shared_budget)--;
    }

    for (size_t end = after; ;
         end += pattern_char_size(search->text, search->text_size, end)) {
        if (pattern_search(search, index + 1, end)) {
            if (search->captures != NULL) {
                /* Segment `index` ends where the wildcard after it starts. */
                search->captures[2 * index] = after;
                search->captures[2 * index + 1] = end - after;
            }
            return 1;
        }
        if (end >= search->text_size) {
            return 0;
        }
    }
}

/*
 * Match a whole pattern against a whole text.  On a match, fills `captures`
 * with `star_count` (offset, size) pairs, one per wildcard in order of
 * appearance, and returns 1; otherwise returns 0 and leaves `captures` alone.
 * Pass NULL for `captures` to ask whether the pattern matches at all.
 *
 * `shared_budget` points at the positions left for the whole lookup, or is
 * NULL for a single check; both budgets are spent together and running out of
 * either one counts as "no match".
 */
static int
pattern_match(const char *pattern, size_t pattern_size,
              const char *text, size_t text_size,
              size_t star_count, size_t *shared_budget, size_t *captures)
{
    PatternSearch search;
    search.text = text;
    search.text_size = text_size;
    search.pattern = pattern;
    search.pattern_size = pattern_size;
    search.star_count = star_count;
    search.captures = captures;
    search.budget = MESSAGE_PATTERN_BUDGET;
    search.shared_budget = shared_budget;
    return pattern_search(&search, 0, 0);
}

/*
 * Read the capture reference at replacement[*position], which is a '%'
 * followed by one or more decimal digits.
 *
 * Returns 0 when there is no reference here, so the '%' is literal text; -1
 * when the reference is out of range for a pattern with `star_count`
 * wildcards, which makes the whole replacement unusable; and the capture
 * number otherwise, leaving *position after the digits.
 */
static long
pattern_reference(const char *replacement, size_t replacement_size,
                  size_t *position, size_t star_count)
{
    size_t i = *position + 1;   /* step over the '%' */
    if (i >= replacement_size
        || replacement[i] < '0' || replacement[i] > '9') {
        return 0;
    }

    size_t value = 0;
    while (i < replacement_size
           && replacement[i] >= '0' && replacement[i] <= '9') {
        value = value * 10 + (size_t)(replacement[i] - '0');
        if (value > star_count) {
            /* More digits can only make it larger. */
            return -1;
        }
        i++;
    }
    *position = i;
    if (value == 0) {
        return -1;              /* captures are numbered from 1 */
    }
    return (long)value;
}

/*
 * Build the replacement text: every "%n" in `replacement` becomes the run the
 * n-th wildcard captured in `text`.
 *
 * Returns 1 with a malloc'ed NUL-terminated buffer in *result and its length
 * in *result_size, or 0 with *result NULL when the replacement names a capture
 * the pattern does not have.  Callers then keep the original message: a
 * reference that cannot be resolved must not silently delete text from it.
 */
static int
pattern_expand(const char *replacement, size_t replacement_size,
               const char *text, const size_t *captures, size_t star_count,
               char **result, size_t *result_size)
{
    *result = NULL;
    *result_size = 0;

    /* First pass: check every reference and measure the result. */
    size_t size = 0;
    size_t position = 0;
    while (position < replacement_size) {
        if (replacement[position] != MESSAGE_PATTERN_REFERENCE) {
            position++;
            size++;
            continue;
        }
        size_t after = position;
        long reference = pattern_reference(replacement, replacement_size,
                                           &after, star_count);
        if (reference < 0) {
            return 0;
        }
        if (reference == 0) {
            position++;         /* a literal '%' */
            size++;
            continue;
        }
        size += captures[2 * (size_t)(reference - 1) + 1];
        position = after;
    }

    char *buffer = malloc(size + 1);
    if (buffer == NULL) {
        return 0;
    }

    /* Second pass: the same walk, copying. */
    size_t written = 0;
    position = 0;
    while (position < replacement_size) {
        if (replacement[position] != MESSAGE_PATTERN_REFERENCE) {
            buffer[written++] = replacement[position++];
            continue;
        }
        size_t after = position;
        long reference = pattern_reference(replacement, replacement_size,
                                           &after, star_count);
        if (reference <= 0) {
            buffer[written++] = replacement[position++];
            continue;
        }
        size_t offset = captures[2 * (size_t)(reference - 1)];
        size_t length = captures[2 * (size_t)(reference - 1) + 1];
        memcpy(buffer + written, text + offset, length);
        written += length;
        position = after;
    }
    buffer[written] = '\0';

    *result = buffer;
    *result_size = written;
    return 1;
}

/* ------------------------------------------------------------------ */
/* table lines (no Python API)                                         */
/* ------------------------------------------------------------------ */

/*
 * One entry per line is "<key>\t<hex-encoded value>": a TAB separates the two
 * halves, and nothing else does.  A message can therefore hold commas, spaces,
 * quotes and parentheses without any escaping, and the only characters a key
 * cannot contain are the TAB (it is the separator) and the newline (it ends
 * the line).
 */
#define MESSAGE_TABLE_SEPARATOR '\t'
#define MESSAGE_TABLE_COMMENT '#'

/* Blanks a line may be padded with, on either side of the separator. */
static int
table_blank(int ch)
{
    return ch == ' ' || ch == '\t' || ch == '\r' || ch == '\n';
}

/*
 * Split one line of `line_size` bytes into its key and its hex-encoded value,
 * in place: the line is cut at the first TAB, and both halves are trimmed, so
 * extra blanks around the separator and a trailing TAB are harmless.
 *
 * The line is handled by its length, never by strlen(): a table line may hold
 * a NUL byte, and treating one as the end of the line would cut an entry in
 * half invisibly.  A NUL anywhere in the line makes it malformed instead.
 *
 * Presence of the TAB decides what the line is, so a message that begins with
 * '#' is safe: "# channels not specified" is one CPython prints, and the old
 * "starts with '#' => comment" rule dropped it silently.  A line without a TAB
 * is a comment when it starts with '#', and malformed otherwise.
 *
 * Returns 1 when the line holds an entry and fills *key and *hex with pointers
 * into it, 0 for a line to ignore (blank or a '#' comment), and -1 for a
 * malformed line: one without a TAB that is not a comment, one holding a NUL,
 * or one with an empty key.  An empty value is an entry, not a malformed line:
 * mapping a message to nothing is how a table silences it.  Callers skip
 * anything but 1, which keeps a broken table from stopping the interpreter
 * from printing the original message.
 *
 * Both pointers are NUL-terminated, so a caller that wants the lengths of the
 * two halves has to measure them -- which the loader does with strlen() on
 * purpose: a NUL inside the line was rejected above.
 */
static int
table_split_line(char *line, size_t line_size, char **key, char **hex)
{
    if (memchr(line, '\0', line_size) != NULL) {
        return -1;
    }

    size_t start = 0;
    size_t end = line_size;
    /* Only the leading blanks are trimmed here.  Trimming the tail first would
       eat a trailing TAB, and "key\t" is exactly how an empty replacement is
       written: the halves are trimmed separately below. */
    while (start < end && table_blank(line[start])) {
        start++;
    }
    while (end > start && table_blank(line[end - 1])
           && line[end - 1] != MESSAGE_TABLE_SEPARATOR) {
        end--;
    }
    if (start == end) {
        return 0;                   /* blank */
    }

    char *text = line + start;
    size_t text_size = end - start;
    char *separator = memchr(text, MESSAGE_TABLE_SEPARATOR, text_size);
    if (separator == NULL) {
        /* No TAB: a comment if it starts with '#', otherwise malformed. */
        return (text[0] == MESSAGE_TABLE_COMMENT) ? 0 : -1;
    }

    size_t key_size = (size_t)(separator - text);
    char *value = separator + 1;
    size_t value_size = text_size - key_size - 1;
    while (key_size > 0 && table_blank(text[key_size - 1])) {
        key_size--;
    }
    while (value_size > 0 && table_blank(value[0])) {
        value++;
        value_size--;
    }
    while (value_size > 0 && table_blank(value[value_size - 1])) {
        value_size--;
    }
    if (key_size == 0) {
        return -1;
    }
    text[key_size] = '\0';
    value[value_size] = '\0';
    *key = text;
    *hex = value;
    return 1;
}

/* ------------------------------------------------------------------ */
/* standalone test harness                                             */
/* ------------------------------------------------------------------ */

#ifdef MESSAGE_TABLE_NO_PYTHON

typedef struct {
    const char *key;
    size_t stars;                       /* expected wildcard count */
} StarCountCase;

/* A key is a pattern exactly when it holds at least one '*'. */
static const StarCountCase star_count_cases[] = {
    {"division by zero", 0},
    {"100% done", 0},                   /* '%' is not a wildcard */
    {"[Errno 2] No such file or directory: '/nope'", 0},
    {"", 0},
    {"'*' object has no attribute '*'", 2},
    {"*", 1},
    {"*: '*' and '*'", 3},
    {"100% *", 1},
};

typedef struct {
    const char *pattern;
    const char *text;
    int matches;                        /* expected match result */
    const char *captures[4];            /* expected runs; "" is an empty run */
    const char *replacement;            /* NULL: do not expand */
    const char *expanded;               /* expected expansion; NULL: refused */
} PatternCase;

/*
 * Text and patterns hold non-ASCII as \x escapes on purpose: the wildcard must
 * never split a UTF-8 sequence, and escaping keeps the cases readable and this
 * file ASCII-only.  "" means an empty run, so a case that expects no captures
 * at all leaves every entry NULL.
 */
static const PatternCase pattern_cases[] = {
    /* A pattern without a wildcard is an ordinary whole-string match. */
    {"abc", "abc", 1, {NULL}, NULL, NULL},
    {"abc", "abc", 1, {NULL}, "xyz", "xyz"},     /* no reference: as it is */
    {"abc", "abc", 1, {NULL}, "%1", NULL},          /* no capture to name */
    {"abc", "xabc", 0, {NULL}, NULL, NULL},
    {"abc", "abcd", 0, {NULL}, NULL, NULL},
    {"", "", 1, {NULL}, NULL, NULL},
    {"", "x", 0, {NULL}, NULL, NULL},

    /* A lone wildcard takes everything, including nothing. */
    {"*", "", 1, {""}, "", ""},
    {"*", "abc", 1, {"abc"}, "100% of %1", "100% of abc"},
    {"*", "abc", 1, {"abc"}, "%1 and %2", NULL},    /* no second capture */
    {"*", "abc", 1, {"abc"}, "%0", NULL},           /* captures start at 1 */
    {"*", "abc", 1, {"abc"}, "%", "%"},       /* a trailing '%' is a literal */
    {"*", "50% off", 1, {"50% off"}, "[%1]", "[50% off]"},

    /* Non-greedy: the first wildcard takes the shortest run that works. */
    {"*a*", "xaxa", 1, {"x", "xa"}, NULL, NULL},
    {"*b*", "abcb", 1, {"a", "cb"}, NULL, NULL},
    {"*a*", "a", 1, {"", ""}, NULL, NULL},
    {"*a*a*", "aaa", 1, {"", "", "a"}, NULL, NULL},

    /* The wildcards are anchored: the ends of the pattern are the ends of the
       text.  An empty run is possible wherever a wildcard meets a literal. */
    {"*c", "abc", 1, {"ab"}, NULL, NULL},
    {"*c", "abcd", 0, {NULL}, NULL, NULL},
    {"a*", "abc", 1, {"bc"}, NULL, NULL},
    {"b*", "ab", 0, {NULL}, NULL, NULL},
    {"*x", "x", 1, {""}, NULL, NULL},

    /* Several wildcards, and a literal '*' inside the text being matched. */
    {"*: '*' and '*'",
     "unsupported operand type(s) for *: 'int' and 'float'", 1,
     {"unsupported operand type(s) for *", "int", "float"},
     "%3 and %2 do not support %1",
     "float and int do not support unsupported operand type(s) for *"},
    {"*: '*' and '*'", "'int' and 'float'", 0, {NULL}, NULL, NULL},
    {"100% *", "100% done", 1, {"done"}, "100% %1", "100% done"},

    /* Multi-byte characters: U+03B1 (two bytes), U+20AC (three bytes) and
       U+1F40D (four bytes) must survive a wildcard on either side. */
    {"*\xce\xb1*", "\xce\xb1\xce\xb1", 1, {"", "\xce\xb1"},
     "<%1%2>", "<\xce\xb1>"},
    {"*\xce\xb1*", "A\xce\xb1" "B", 1, {"A", "B"}, "%2-%1", "B-A"},
    {"*\xe2\x82\xac*", "cost \xe2\x82\xac" " 5", 1, {"cost ", " 5"},
     NULL, NULL},
    {"*\xf0\x9f\x90\x8d*", "a\xf0\x9f\x90\x8d", 1, {"a", ""}, NULL, NULL},
    /* A byte that is not valid UTF-8 counts as one character, never as half of
       one: the matcher has to stay total even on input it does not expect. */
    {"*a*", "\xff" "a" "\xff", 1, {"\xff", "\xff"}, NULL, NULL},

    /* A key may hold a comma: the separator is a TAB, nothing else. */
    {"byte must be in range(0, 256)", "byte must be in range(0, 256)", 1,
     {NULL}, NULL, NULL},
    {"minute, second, and microsecond must be 0 when hour is 24",
     "minute, second, and microsecond must be 0 when hour is 24", 1,
     {NULL}, NULL, NULL},
    {"character mapping must return integer, None or str",
     "character mapping must return integer, None or str", 1,
     {NULL}, NULL, NULL},
    {"Hour must be in [0, *]", "Hour must be in [0, 167]", 1, {"167"},
     "Hour must be in [0, %1]", "Hour must be in [0, 167]"},
};

typedef struct {
    const char *line;               /* the line as it appears in the file */
    int verdict;                    /* expected table_split_line() result */
    const char *key;                /* expected key, when verdict is 1 */
    const char *hex;                /* expected value, when verdict is 1 */
} SplitCase;

/*
 * The separator is a TAB, so a key may hold a comma: the first cases are
 * messages the old comma-separated format could not carry at all, and the rest
 * are the shapes a line can take around the separator, including the ones that
 * have to be ignored (0) or rejected (-1).
 */
static const SplitCase split_cases[] = {
    {"byte must be in range(0, 256)\t62797465", 1,
     "byte must be in range(0, 256)", "62797465"},
    {"minute, second, and microsecond must be 0 when hour is 24\t6162", 1,
     "minute, second, and microsecond must be 0 when hour is 24", "6162"},
    {"Hour must be in [0, *]\t*", 1, "Hour must be in [0, *]", "*"},

    /* Blanks around the separator, a second TAB, a CRLF and a '#' that is not
       at the start of the line. */
    {" division by zero \t e7aca8 ", 1, "division by zero", "e7aca8"},
    {"division by zero\t\te7aca8", 1, "division by zero", "e7aca8"},
    {"division by zero\te7aca8\t", 1, "division by zero", "e7aca8"},
    {"division by zero\te7aca8\r\n", 1, "division by zero", "e7aca8"},
    {"a # b\t00", 1, "a # b", "00"},
    /* A message may start with '#'.  CPython prints "# channels not specified",
       and the old "starts with '#' => comment" rule dropped it silently, so the
       TAB decides: with one the line is an entry, without one it is a comment. */
    {"# channels not specified\t6162", 1, "# channels not specified", "6162"},
    {"#not a comment because of the TAB\t00", 1, "#not a comment because of the TAB", "00"},

    /* Lines to ignore. */
    {"", 0, NULL, NULL},
    {"   ", 0, NULL, NULL},
    {"\t", 0, NULL, NULL},
    {"# comment", 0, NULL, NULL},
    /* '#' with a TAB is an entry now, not a comment: key "# comment", value
       "with a TAB" (which is not hex, so the loader drops it as a bad value --
       the splitter's job is only to hand the halves over). */
    {"   # comment\twith a TAB", 1, "# comment", "with a TAB"},

    /* Malformed lines: no TAB (which includes the old comma format) and an
       empty key.  Leading blanks are trimmed before the separator is looked
       for, so a line that starts with the separator reaches this branch as
       "no separator" instead. */
    {"byte must be in range(0, 256)", -1, NULL, NULL},
    {"division by zero,e7aca8", -1, NULL, NULL},
    {"\te7aca8", -1, NULL, NULL},
    {"   \te7aca8", -1, NULL, NULL},

    /* An empty value is an entry: it maps the message to nothing, which is how
       a table silences a message on purpose.  Only the key has to be there. */
    {"division by zero\t", 1, "division by zero", ""},
    {"division by zero\t   ", 1, "division by zero", ""},
    {"division by zero\t\t\t", 1, "division by zero", ""},
};

static int pattern_test_checks = 0;
static int pattern_test_failures = 0;

/* Print bytes so that the output is ASCII and stays comparable anywhere. */
static void
print_bytes(const char *bytes, size_t size)
{
    for (size_t i = 0; i < size; i++) {
        unsigned char byte = (unsigned char)bytes[i];
        if (byte >= 0x20 && byte < 0x7f) {
            putchar((int)byte);
        }
        else {
            printf("\\x%02x", (unsigned int)byte);
        }
    }
}

static void
print_quoted(const char *bytes)
{
    putchar('"');
    print_bytes(bytes, strlen(bytes));
    putchar('"');
}

/*
 * One line of the table: the verdict, and for an entry the two halves it split
 * into.  The separator shows up as \x09 in the printed line.
 */
static void
check_split(const SplitCase *test)
{
    char buffer[MESSAGE_TABLE_MAX_LINE];
    snprintf(buffer, sizeof(buffer), "%s", test->line);
    char *key = NULL;
    char *hex = NULL;
    int verdict = table_split_line(buffer, strlen(buffer), &key, &hex);

    pattern_test_checks++;
    printf("      line=");
    print_quoted(test->line);
    printf(" verdict=%d", verdict);

    int failed = (verdict != test->verdict);
    if (verdict == 1) {
        printf(" key=");
        print_quoted(key);
        printf(" hex=");
        print_quoted(hex);
        if (test->key == NULL || strcmp(key, test->key) != 0) {
            printf("(expected key ");
            print_quoted(test->key != NULL ? test->key : "");
            putchar(')');
            failed = 1;
        }
        if (test->hex == NULL || strcmp(hex, test->hex) != 0) {
            printf("(expected hex ");
            print_quoted(test->hex != NULL ? test->hex : "");
            putchar(')');
            failed = 1;
        }
    }
    putchar('\n');
    if (failed) {
        pattern_test_failures++;
        printf("FAIL  split case with expected verdict %d\n", test->verdict);
    }
}

static void
check_star_count(const StarCountCase *test)
{
    size_t stars = pattern_star_count(test->key, strlen(test->key));
    pattern_test_checks++;
    if (stars != test->stars) {
        pattern_test_failures++;
        printf("FAIL  star count of ");
        print_quoted(test->key);
        printf(" is %lu, expected %lu\n",
               (unsigned long)stars, (unsigned long)test->stars);
    }
    else {
        printf("ok    star count of ");
        print_quoted(test->key);
        printf(" is %lu\n", (unsigned long)stars);
    }
}

static void
check_case(const PatternCase *test)
{
    size_t pattern_size = strlen(test->pattern);
    size_t text_size = strlen(test->text);
    size_t stars = pattern_star_count(test->pattern, pattern_size);
    size_t captures[2 * MESSAGE_PATTERN_MAX_STARS];
    int matched = pattern_match(test->pattern, pattern_size,
                                test->text, text_size, stars, NULL, captures);

    pattern_test_checks++;
    printf("      pattern=");
    print_quoted(test->pattern);
    printf(" text=");
    print_quoted(test->text);
    printf(" stars=%lu match=%d", (unsigned long)stars, matched);

    int failed = (matched != test->matches);
    if (matched) {
        for (size_t i = 0; i < stars; i++) {
            const char *expected = test->captures[i];
            size_t offset = captures[2 * i];
            size_t size = captures[2 * i + 1];
            printf(" cap%lu=", (unsigned long)(i + 1));
            print_bytes(test->text + offset, size);
            if (expected == NULL
                || strlen(expected) != size
                || memcmp(expected, test->text + offset, size) != 0) {
                printf("(expected ");
                print_quoted(expected != NULL ? expected : "");
                putchar(')');
                failed = 1;
            }
        }
    }

    if (test->replacement != NULL) {
        char *expanded = NULL;
        size_t expanded_size = 0;
        int ok = pattern_expand(test->replacement, strlen(test->replacement),
                                test->text, captures, stars,
                                &expanded, &expanded_size);
        printf(" expanded=");
        if (!ok) {
            printf("REFUSED");
        }
        else {
            print_quoted(expanded);
        }
        if (test->expanded == NULL) {
            if (ok) {
                printf("(expected REFUSED)");
                failed = 1;
            }
        }
        else if (!ok
                 || strlen(test->expanded) != expanded_size
                 || memcmp(test->expanded, expanded, expanded_size) != 0) {
            printf("(expected ");
            print_quoted(test->expanded);
            putchar(')');
            failed = 1;
        }
        free(expanded);
    }

    putchar('\n');
    if (failed) {
        pattern_test_failures++;
        printf("FAIL  case %lu\n", (unsigned long)pattern_test_checks);
    }
}

/* Decode a hex field of the stdin protocol; NULL on anything malformed. */
static char *
test_from_hex(const char *hex, size_t *size)
{
    size_t length = strlen(hex);
    if (length % 2 != 0) {
        return NULL;
    }
    char *bytes = malloc(length / 2 + 1);
    if (bytes == NULL) {
        return NULL;
    }
    for (size_t i = 0; i < length / 2; i++) {
        int value = 0;
        for (int half = 0; half < 2; half++) {
            int digit = hex[2 * i + half];
            value <<= 4;
            if (digit >= '0' && digit <= '9') {
                value |= digit - '0';
            }
            else if (digit >= 'a' && digit <= 'f') {
                value |= digit - 'a' + 10;
            }
            else if (digit >= 'A' && digit <= 'F') {
                value |= digit - 'A' + 10;
            }
            else {
                free(bytes);
                return NULL;
            }
        }
        bytes[i] = (char)value;
    }
    bytes[length / 2] = '\0';
    *size = length / 2;
    return bytes;
}

static void
print_hex(const char *bytes, size_t size)
{
    for (size_t i = 0; i < size; i++) {
        printf("%02x", (unsigned int)(unsigned char)bytes[i]);
    }
}

/*
 * Cases from stdin, one per line, hex-encoded and tab separated:
 *
 *     <pattern-hex> \t <text-hex> \t <replacement-hex>
 *
 * The third field is optional; without it the line only asks for match and
 * captures.  The answer is one line per case:
 *
 *     <match 0|1> \t <stars> \t <capture-hex>... \t <-|REFUSED|OK:hex>
 */
static int
run_stdin_cases(void)
{
    char line[3 * (int)MESSAGE_TABLE_MAX_LINE + 8];
    int failures = 0;
    int cases = 0;

    while (fgets(line, (int)sizeof(line), stdin) != NULL) {
        size_t length = strlen(line);
        while (length > 0
               && (line[length - 1] == '\n' || line[length - 1] == '\r')) {
            line[--length] = '\0';
        }
        if (length == 0) {
            continue;
        }
        char *fields[3] = {line, NULL, NULL};
        int field_count = 1;
        for (char *at = line; *at != '\0' && field_count < 3; at++) {
            if (*at == '\t') {
                *at = '\0';
                fields[field_count++] = at + 1;
            }
        }

        size_t pattern_size = 0, text_size = 0, replacement_size = 0;
        char *pattern = test_from_hex(fields[0], &pattern_size);
        char *text = test_from_hex(fields[1], &text_size);
        char *replacement = (field_count == 3)
            ? test_from_hex(fields[2], &replacement_size) : NULL;
        if (pattern == NULL || text == NULL
            || (field_count == 3 && replacement == NULL)) {
            printf("ERROR malformed case\n");
            failures++;
            free(pattern);
            free(text);
            free(replacement);
            continue;
        }

        cases++;
        size_t stars = pattern_star_count(pattern, pattern_size);
        size_t captures[2 * MESSAGE_PATTERN_MAX_STARS];
        int matched = pattern_match(pattern, pattern_size, text, text_size,
                                    stars, NULL, captures);
        printf("%d\t%lu", matched, (unsigned long)stars);
        if (matched) {
            for (size_t i = 0; i < stars; i++) {
                putchar('\t');
                print_hex(text + captures[2 * i], captures[2 * i + 1]);
            }
        }
        if (field_count < 3) {
            printf("\t-\n");
        }
        else {
            char *expanded = NULL;
            size_t expanded_size = 0;
            int expanded_ok = matched
                && pattern_expand(replacement, replacement_size, text,
                                  captures, stars, &expanded, &expanded_size);
            if (expanded_ok) {
                printf("\tOK:");
                print_hex(expanded, expanded_size);
                putchar('\n');
            }
            else {
                printf("\tREFUSED\n");
            }
            free(expanded);
        }

        free(pattern);
        free(text);
        free(replacement);
    }

    fprintf(stderr, "%d stdin cases, %d malformed\n", cases, failures);
    return failures == 0 ? 0 : 1;
}

/*
 * Lines from stdin, one per line, answered with table_split_line()'s verdict
 * and, for an entry, the key and the value as hex:
 *
 *     <verdict 1|0|-1> \t <key-hex> \t <value-hex>
 */
static int
run_split_cases(void)
{
    char line[MESSAGE_TABLE_MAX_LINE];

    while (fgets(line, (int)sizeof(line), stdin) != NULL) {
        /* fgets() keeps the newline; the splitter trims it off. */
        char *key = NULL;
        char *hex = NULL;
        int verdict = table_split_line(line, strlen(line), &key, &hex);
        printf("%d", verdict);
        putchar('\t');
        if (verdict == 1) {
            print_hex(key, strlen(key));
            putchar('\t');
            print_hex(hex, strlen(hex));
        }
        putchar('\n');
    }
    return 0;
}

/*
 * Lines that cannot be written as a plain C string for the splitter's table
 * above: a NUL byte is part of the line and makes it malformed rather than
 * ending it, so these cases hand the length over explicitly.
 */
static void
check_raw_split_cases(void)
{
    static const struct {
        const char *bytes;
        size_t size;
        int verdict;
    } cases[] = {
        {"division by zero\te7\x00" "aca8",
         sizeof("division by zero\te7\x00" "aca8") - 1, -1},
        {"divi\x00" "sion by zero\te7aca8",
         sizeof("divi\x00" "sion by zero\te7aca8") - 1, -1},
        {"division by zero\t", sizeof("division by zero\t") - 1, 1},
        {"division by zero\t\x00", sizeof("division by zero\t\x00") - 1, -1},
    };

    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        char buffer[MESSAGE_TABLE_MAX_LINE];
        char *key = NULL;
        char *hex = NULL;
        memcpy(buffer, cases[i].bytes, cases[i].size);
        int verdict = table_split_line(buffer, cases[i].size, &key, &hex);
        pattern_test_checks++;
        printf("      raw line of %lu bytes: verdict=%d (expected %d)\n",
               (unsigned long)cases[i].size, verdict, cases[i].verdict);
        if (verdict != cases[i].verdict) {
            pattern_test_failures++;
            printf("FAIL  raw split case %lu\n", (unsigned long)(i + 1));
        }
    }
}

int
main(int argc, char **argv)
{
    if (argc > 1 && strcmp(argv[1], "-") == 0) {
        return run_stdin_cases();
    }
    if (argc > 1 && strcmp(argv[1], "--split") == 0) {
        return run_split_cases();
    }

    printf("star counts\n");
    for (size_t i = 0;
         i < sizeof(star_count_cases) / sizeof(star_count_cases[0]); i++) {
        check_star_count(&star_count_cases[i]);
    }

    printf("table lines\n");
    for (size_t i = 0; i < sizeof(split_cases) / sizeof(split_cases[0]); i++) {
        check_split(&split_cases[i]);
    }
    check_raw_split_cases();

    printf("matching and expansion\n");
    for (size_t i = 0;
         i < sizeof(pattern_cases) / sizeof(pattern_cases[0]); i++) {
        check_case(&pattern_cases[i]);
    }

    printf("\n%lu checks, %lu failures\n",
           (unsigned long)pattern_test_checks,
           (unsigned long)pattern_test_failures);
    return pattern_test_failures == 0 ? 0 : 1;
}

#endif /* MESSAGE_TABLE_NO_PYTHON */

#ifndef MESSAGE_TABLE_NO_PYTHON

/* ------------------------------------------------------------------ */
/* table                                                               */
/* ------------------------------------------------------------------ */

typedef struct {
    PyObject *key;              /* original message or pattern, a str */
    PyObject *value;            /* replacement text, a str */
    const char *key_utf8;       /* key as UTF-8, owned by `key`; NULL for a
                                   literal key */
    Py_ssize_t key_utf8_size;
    const char *value_utf8;     /* replacement as UTF-8, owned by `value`;
                                   NULL for a literal key */
    Py_ssize_t value_utf8_size;
    size_t star_count;          /* 0 when `key` is a literal message */
} MessageEntry;

typedef struct {
    PyObject *literals;         /* dict: message -> replacement, no wildcard */
    MessageEntry *patterns;     /* entries whose key holds at least one '*' */
    Py_ssize_t pattern_count;
    Py_ssize_t pattern_capacity;
    Py_ssize_t entry_count;     /* literals plus patterns */
    Py_ssize_t lines_skipped;   /* malformed lines, for the diagnostic */
    Py_ssize_t lines_too_long;  /* lines past MESSAGE_TABLE_MAX_LINE */
} MessageTable;

#define MESSAGE_TABLE_ENV "PYTHON_MESSAGE_TABLE"
#define MESSAGE_TABLE_DEFAULT "lookup-table.txt"

/*
 * The loaded table is a capsule in the interpreter's own dict, and the proxy
 * cache a list beside it.  Two properties come out of that, both of them
 * requirements rather than conveniences:
 *
 *   - Per interpreter.  The table holds str objects and the proxy cache holds
 *     types, so sharing them between interpreters would hand objects owned by
 *     one interpreter to another (and, with per-interpreter allocators, free
 *     them from the wrong one).  A subinterpreter now builds its own table and
 *     its own proxies, and both die with the interpreter's dict.
 *   - No lock needed.  The capsule's payload is immutable once published, so a
 *     reader only has to keep the capsule alive, which PyDict_GetItemRef() and
 *     PyDict_SetDefaultRef() do atomically even without the GIL.  The proxy
 *     list is a Python list: appending to it cannot corrupt another reader, and
 *     a duplicate entry under a race only costs one extra class.
 */
#define MESSAGE_TABLE_STATE_KEY "_PyMessageTable_table"
#define MESSAGE_TABLE_PROXIES_KEY "_PyMessageTable_proxies"
#define MESSAGE_TABLE_CAPSULE_NAME "kawaii-python.message_table"

/*
 * Caps on what the loader will take from a path that may be under someone
 * else's control.  The table is read while an exception is being displayed, so
 * a table that is enormous, or that is not an ordinary file at all, must not be
 * able to hold the interpreter there: the size check happens before a byte is
 * read, and the entry cap bounds what is kept.
 */
#define MESSAGE_TABLE_MAX_BYTES (16 * 1024 * 1024)
#define MESSAGE_TABLE_MAX_ENTRIES 65536

/* Which candidate the table came from, for the diagnostic. */
typedef enum {
    MESSAGE_TABLE_SOURCE_ENV = 0,
    MESSAGE_TABLE_SOURCE_HOME,
    MESSAGE_TABLE_SOURCE_PREFIX,
    MESSAGE_TABLE_SOURCE_CWD,
    MESSAGE_TABLE_SOURCE_COUNT
} MessageTableSource;

/*
 * The interpreter dict our state lives in, borrowed, or NULL when the
 * interpreter has no dict (which is not an error: without a place to keep the
 * cache there simply is no replacement to apply).
 */
static PyObject *
message_table_interp_dict(void)
{
    PyInterpreterState *interp = PyInterpreterState_Get();
    if (interp == NULL) {
        return NULL;
    }
    return PyInterpreterState_GetDict(interp);
}

/* Release everything a table owns. */
static void
message_table_free(MessageTable *table)
{
    if (table == NULL) {
        return;
    }
    Py_XDECREF(table->literals);
    for (Py_ssize_t i = 0; i < table->pattern_count; i++) {
        Py_XDECREF(table->patterns[i].key);
        Py_XDECREF(table->patterns[i].value);
    }
    PyMem_Free(table->patterns);
    PyMem_Free(table);
}

/* Free the table a capsule owns; the capsule API hands over the pointer. */
static void
message_table_capsule_free(PyObject *capsule)
{
    MessageTable *table = PyCapsule_GetPointer(capsule,
                                               MESSAGE_TABLE_CAPSULE_NAME);
    if (table == NULL) {
        PyErr_Clear();
        return;
    }
    message_table_free(table);
}

/* ------------------------------------------------------------------ */
/* where the table lives                                               */
/* ------------------------------------------------------------------ */

/* One -X option: "off" switches message replacement off for this run. */
#define MESSAGE_TABLE_XOPTION "message-table"
#define MESSAGE_TABLE_XOPTION_OFF "off"

/*
 * sys.<name> as a truth value, 0 when it is missing or false.  The kernel of
 * the interpreter is running while this is called, so a missing attribute is a
 * "no", and the error indicator is left exactly as it was found.
 */
static int
message_table_sys_flag(const char *name)
{
    PyObject *flags = PySys_GetObject("flags");
    if (flags == NULL) {
        PyErr_Clear();
        return 0;
    }
    PyObject *value = PyObject_GetAttrString(flags, name);
    if (value == NULL) {
        PyErr_Clear();
        return 0;
    }
    int set = PyObject_IsTrue(value);
    Py_DECREF(value);
    if (set < 0) {
        PyErr_Clear();
        return 0;
    }
    return set;
}

/* 1 when -X message-table=off was given. */
static int
message_table_switched_off(void)
{
    PyObject *options = PySys_GetObject("_xoptions");
    if (options == NULL) {
        PyErr_Clear();
        return 0;
    }
    if (!PyDict_Check(options)) {
        return 0;
    }
    PyObject *value = NULL;
    if (PyDict_GetItemStringRef(options, MESSAGE_TABLE_XOPTION, &value) < 0) {
        PyErr_Clear();
        return 0;
    }
    if (value == NULL) {
        return 0;
    }
    int off = PyUnicode_Check(value)
        && PyUnicode_CompareWithASCIIString(value,
                                            MESSAGE_TABLE_XOPTION_OFF) == 0;
    Py_DECREF(value);
    PyErr_Clear();                  /* the comparison may have failed */
    return off;
}

/*
 * The value of an environment variable as text, or NULL when it is unset or
 * empty.
 *
 * On Windows the narrow getenv() hands back bytes in the ANSI code page, which
 * cannot represent a path outside that code page at all: the characters are
 * already '?' by the time any decoding could help, so a table under such a path
 * would be read as "not there".  The wide environment is the one that holds
 * what the user set, which is why the interpreter reads its own variables that
 * way.  Elsewhere the bytes are in the locale encoding and
 * PyUnicode_DecodeLocale() turns them into text.
 *
 * This does not consult the "ignore environment" flag: the caller decides
 * whether a variable may be read at all (see message_table_candidate_wanted).
 */
static PyObject *
message_table_env(const char *name, const wchar_t *wname)
{
#ifdef MS_WINDOWS
    wchar_t *value = _wgetenv(wname);
    if (value == NULL || value[0] == L'\0') {
        return NULL;
    }
    return PyUnicode_FromWideChar(value, -1);
#else
    const char *value = getenv(name);
    if (value == NULL || value[0] == '\0') {
        return NULL;
    }
    return PyUnicode_DecodeLocale(value, NULL);
#endif
}

/* Defined with the rest of the file handling, further down. */
static FILE *message_table_open(PyObject *path);

/*
 * Is this a source tree rather than an installation?  The table that ships
 * with a release lives in the installation directory, and a source checkout
 * has the deliverable of this project at its root -- the very same file name.
 * Treating the checkout as an installation would rewrite the messages of the
 * test suite that runs from it, so the shipped-table candidate is skipped
 * there.  The marker is the one sysconfig.is_python_build() uses:
 * <prefix>/Modules/Setup (or Setup.local) exists only in a configured source
 * tree.
 */
static int
message_table_source_tree(PyObject *prefix)
{
    static const char *const markers[] = {"Setup", "Setup.local", NULL};
    for (const char *const *name = markers; *name != NULL; name++) {
        PyObject *path = PyUnicode_FromFormat("%U/Modules/%s", prefix, *name);
        if (path == NULL) {
            PyErr_Clear();
            return 0;
        }
        int exists = 0;
        FILE *fp = message_table_open(path);
        if (fp != NULL) {
            exists = 1;
            fclose(fp);
        }
        Py_DECREF(path);
        if (exists) {
            return 1;
        }
    }
    return 0;
}

/*
 * The path of one candidate: <directory>/lookup-table.txt for the candidates
 * that name a directory, the value of the variable for the one that names a
 * file, and the bare file name for the current directory.  Returns a new
 * reference, or NULL.  Whether the candidate may be tried at all is decided by
 * message_table_candidate_wanted(), not here.
 */
static PyObject *
message_table_candidate(MessageTableSource source)
{
    switch (source) {
    case MESSAGE_TABLE_SOURCE_ENV:
        return message_table_env(MESSAGE_TABLE_ENV,
                                 L"" MESSAGE_TABLE_ENV);
    case MESSAGE_TABLE_SOURCE_HOME: {
        PyObject *home = message_table_env("PYTHONHOME", L"PYTHONHOME");
        if (home == NULL) {
            return NULL;
        }
        PyObject *path = PyUnicode_FromFormat("%U/%s", home,
                                              MESSAGE_TABLE_DEFAULT);
        Py_DECREF(home);
        return path;
    }
    case MESSAGE_TABLE_SOURCE_PREFIX: {
        PyObject *prefix = PySys_GetObject("prefix");
        if (prefix == NULL || !PyUnicode_Check(prefix)) {
            PyErr_Clear();
            return NULL;
        }
        if (message_table_source_tree(prefix)) {
            return NULL;
        }
        return PyUnicode_FromFormat("%U/%s", prefix, MESSAGE_TABLE_DEFAULT);
    }
    case MESSAGE_TABLE_SOURCE_CWD:
        return PyUnicode_FromString(MESSAGE_TABLE_DEFAULT);
    default:
        return NULL;
    }
}

/*
 * Is this candidate to be tried?  -X message-table=off drops all of them, -E
 * drops the two that come from the environment and -I (which implies -E) drops
 * the current directory as well: an isolated interpreter must not have its
 * error messages rewritten by a file somebody else can write.
 */
static int
message_table_candidate_wanted(MessageTableSource source)
{
    if (message_table_switched_off()) {
        return 0;
    }
    int ignore_environment = message_table_sys_flag("ignore_environment");
    int isolated = message_table_sys_flag("isolated");
    switch (source) {
    case MESSAGE_TABLE_SOURCE_ENV:
    case MESSAGE_TABLE_SOURCE_HOME:
        return !ignore_environment;
    case MESSAGE_TABLE_SOURCE_CWD:
        return !isolated;
    case MESSAGE_TABLE_SOURCE_PREFIX:
        return 1;
    default:
        return 0;
    }
}

/* ------------------------------------------------------------------ */
/* hex decoding                                                        */
/* ------------------------------------------------------------------ */

static int
hex_digit(int ch)
{
    if (ch >= '0' && ch <= '9') {
        return ch - '0';
    }
    if (ch >= 'a' && ch <= 'f') {
        return ch - 'a' + 10;
    }
    if (ch >= 'A' && ch <= 'F') {
        return ch - 'A' + 10;
    }
    return -1;
}

/*
 * Decode a hex field of `hex_size` bytes into a str by way of its UTF-8 bytes.
 *
 * Validates the input: an odd length, a non-hex character or bytes that are not
 * UTF-8 return NULL with an exception set instead of silently decoding garbage.
 * The length is passed in rather than measured, because the field sits in a
 * line that may hold a NUL byte; the loader rejects such a line before it gets
 * here, and this keeps the function independent of that decision.
 *
 * The decode is strict on purpose: a replacement that is not valid UTF-8 is a
 * broken entry, and dropping the entry keeps the original message visible,
 * which is better than printing U+FFFD in its place.
 *
 * File-local on purpose: exporting it would add a public-looking C API that
 * needs documentation, for no gain -- nothing outside this file calls it.
 */
static PyObject *
from_hex(const char *hex, size_t hex_size)
{
    if (hex == NULL) {
        PyErr_SetString(PyExc_ValueError, "hex string is NULL");
        return NULL;
    }

    if (hex_size % 2 != 0) {
        PyErr_Format(PyExc_ValueError, "hex string of odd length: %zu",
                     hex_size);
        return NULL;
    }

    size_t nbytes = hex_size / 2;
    char *bytes = PyMem_Malloc(nbytes > 0 ? nbytes : 1);
    if (bytes == NULL) {
        PyErr_NoMemory();
        return NULL;
    }

    for (size_t i = 0; i < nbytes; i++) {
        int hi = hex_digit((unsigned char)hex[2 * i]);
        int lo = hex_digit((unsigned char)hex[2 * i + 1]);
        if (hi < 0 || lo < 0) {
            PyMem_Free(bytes);
            PyErr_Format(PyExc_ValueError, "invalid hex digit at offset %zu",
                         2 * i);
            return NULL;
        }
        bytes[i] = (char)((hi << 4) | lo);
    }

    PyObject *result = PyUnicode_DecodeUTF8(bytes, (Py_ssize_t)nbytes,
                                            "strict");
    PyMem_Free(bytes);
    return result;
}

/* ------------------------------------------------------------------ */
/* table loading                                                       */
/* ------------------------------------------------------------------ */

/*
 * Open the table for reading.
 *
 * Deliberately NOT _Py_fopen_obj(): that internal helper asserts a clean thread
 * state and fires a PySys_Audit("open", ...) event.  A lookup triggered by an
 * error message does neither -- it runs while an exception is being handled,
 * and it is not a user-visible file operation.  Path handling mirrors what
 * CPython itself does: wide characters on Windows, filesystem encoding
 * elsewhere.
 */
static FILE *
message_table_open(PyObject *path)
{
#ifdef MESSAGE_TABLE_STUB_BUILD
    const char *utf8 = PyUnicode_AsUTF8AndSize(path, NULL);
    return utf8 != NULL ? fopen(utf8, "rb") : NULL;
#elif defined(MS_WINDOWS)
    wchar_t *wpath = PyUnicode_AsWideCharString(path, NULL);
    if (wpath == NULL) {
        return NULL;
    }
    /* Binary mode matters: in text mode the C runtime treats a 0x1A byte as the
       end of the file, so a table holding one would lose every entry after it
       -- and lose them silently.  Line ends are stripped by the splitter, which
       trims '\r' like any other blank. */
    FILE *fp = _wfopen(wpath, L"rb");
    PyMem_Free(wpath);
    return fp;
#else
    PyObject *bytes = PyUnicode_EncodeFSDefault(path);
    if (bytes == NULL) {
        return NULL;
    }
    FILE *fp = fopen(PyBytes_AS_STRING(bytes), "rb");
    Py_DECREF(bytes);
    return fp;
#endif
}

/*
 * Is this an ordinary file of an acceptable size?  The check happens before a
 * byte is read: the loader runs while an exception is displayed, so a FIFO (a
 * read that never returns until someone writes), a device such as /dev/zero (a
 * read that never ends) or a table far larger than a message table can be must
 * be refused up front rather than discovered halfway through.
 *
 * Returns 0 to refuse the file, 1 to read it.
 */
static int
message_table_size_ok(FILE *fp)
{
#ifdef MESSAGE_TABLE_STUB_BUILD
    return 1;
#else
#ifdef MS_WINDOWS
    struct _stat64 info;
    if (_fstat64(_fileno(fp), &info) != 0) {
        return 0;
    }
    if ((info.st_mode & _S_IFMT) != _S_IFREG) {
        return 0;
    }
    return info.st_size >= 0 && info.st_size <= MESSAGE_TABLE_MAX_BYTES;
#else
    struct stat info;
    if (fstat(fileno(fp), &info) != 0) {
        return 0;
    }
    if (!S_ISREG(info.st_mode)) {
        return 0;
    }
    return info.st_size >= 0 && info.st_size <= MESSAGE_TABLE_MAX_BYTES;
#endif
#endif
}

/*
 * Prepare one entry of the table.
 *
 * A key with no '*' is a literal message and goes into the literals dict, which
 * answers a lookup in one hashed step.  A key with at least one '*' is a
 * pattern: both halves are cached as UTF-8 bytes in the entry, because the
 * pattern engine works on bytes and the cached pointer stays valid as long as
 * the entry holds a reference to the str that owns it.  A key or value that
 * cannot be encoded as UTF-8, and a pattern with more wildcards than
 * MESSAGE_PATTERN_MAX_STARS, are dropped like any other malformed entry.
 */
static void
message_table_add(MessageTable *table, char *key, char *hex)
{
    PyObject *value = from_hex(hex, strlen(hex));
    if (value == NULL) {
        /* A malformed value must never stop the interpreter from printing the
           original message: drop the entry and carry on. */
        PyErr_Clear();
        table->lines_skipped++;
        return;
    }
    PyObject *key_obj = PyUnicode_FromString(key);
    if (key_obj == NULL) {
        Py_DECREF(value);
        PyErr_Clear();
        table->lines_skipped++;
        return;
    }

    if (strchr(key, MESSAGE_PATTERN_WILDCARD) == NULL) {
        /* A literal message.  The first entry for a key wins, which is what the
           scan this replaces did as well. */
        if (PyDict_SetDefault(table->literals, key_obj, value) == NULL) {
            PyErr_Clear();
            table->lines_skipped++;
        }
        else {
            table->entry_count++;
        }
        Py_DECREF(key_obj);
        Py_DECREF(value);
        return;
    }

    MessageEntry entry;
    entry.key = key_obj;
    entry.value = value;
    entry.key_utf8 = NULL;
    entry.key_utf8_size = 0;
    entry.value_utf8 = NULL;
    entry.value_utf8_size = 0;
    entry.star_count = 0;

    Py_ssize_t key_size = 0;
    Py_ssize_t value_size = 0;
    const char *key_utf8 = PyUnicode_AsUTF8AndSize(key_obj, &key_size);
    const char *value_utf8 = (key_utf8 != NULL)
        ? PyUnicode_AsUTF8AndSize(value, &value_size) : NULL;
    if (key_utf8 == NULL || value_utf8 == NULL) {
        PyErr_Clear();
        Py_DECREF(key_obj);
        Py_DECREF(value);
        table->lines_skipped++;
        return;
    }
    size_t stars = pattern_star_count(key_utf8, (size_t)key_size);
    if (stars > MESSAGE_PATTERN_MAX_STARS) {
        Py_DECREF(key_obj);
        Py_DECREF(value);
        table->lines_skipped++;
        return;
    }
    entry.key_utf8 = key_utf8;
    entry.key_utf8_size = key_size;
    entry.value_utf8 = value_utf8;
    entry.value_utf8_size = value_size;
    entry.star_count = stars;

    if (table->pattern_count == table->pattern_capacity) {
        Py_ssize_t bigger_capacity = table->pattern_capacity == 0
            ? 64 : table->pattern_capacity * 2;
        MessageEntry *bigger = PyMem_Realloc(
            table->patterns,
            (size_t)bigger_capacity * sizeof(MessageEntry));
        if (bigger == NULL) {
            /* Keep what we have, drop the rest. */
            PyErr_Clear();
            Py_DECREF(key_obj);
            Py_DECREF(value);
            return;
        }
        table->patterns = bigger;
        table->pattern_capacity = bigger_capacity;
    }
    table->patterns[table->pattern_count] = entry;
    table->pattern_count++;
    table->entry_count++;
}

/*
 * A small buffered reader over a FILE *.
 *
 * Bytes are read a chunk at a time and handed out one by one, which is what
 * lets the line reader work on counts rather than on C strings: fgets() stops
 * at a NUL byte, so a line holding one would come back cut in half without
 * anybody noticing, and a line longer than the buffer would come back as two
 * lines that each look complete.
 */
typedef struct {
    FILE *fp;
    char chunk[4096];
    size_t size;                /* bytes valid in chunk */
    size_t position;
} TableReader;

static void
table_reader_init(TableReader *reader, FILE *fp)
{
    reader->fp = fp;
    reader->size = 0;
    reader->position = 0;
}

/* The next byte, or -1 at end of file. */
static int
table_reader_next(TableReader *reader)
{
    if (reader->position == reader->size) {
        size_t got = fread(reader->chunk, 1, sizeof(reader->chunk),
                           reader->fp);
        if (got == 0) {
            return -1;
        }
        reader->size = got;
        reader->position = 0;
    }
    return (unsigned char)reader->chunk[reader->position++];
}

/*
 * One line, without its terminator, in a buffer that grows as needed and always
 * ends with a NUL past the bytes it holds.  `length` is the number of bytes
 * handed out, which is what the splitter works from.
 */
typedef struct {
    char *data;
    size_t capacity;
    size_t length;
} TableLine;

static int
table_line_init(TableLine *line)
{
    line->capacity = 256;
    line->length = 0;
    line->data = PyMem_Malloc(line->capacity);
    if (line->data == NULL) {
        PyErr_NoMemory();
        return -1;
    }
    return 0;
}

static void
table_line_free(TableLine *line)
{
    PyMem_Free(line->data);
    line->data = NULL;
    line->capacity = 0;
    line->length = 0;
}

/* Make room for `needed` bytes including the terminating NUL. */
static int
table_line_reserve(TableLine *line, size_t needed)
{
    if (needed <= line->capacity) {
        return 0;
    }
    size_t capacity = line->capacity;
    while (capacity < needed) {
        capacity *= 2;
    }
    char *bigger = PyMem_Realloc(line->data, capacity);
    if (bigger == NULL) {
        PyErr_NoMemory();
        return -1;
    }
    line->data = bigger;
    line->capacity = capacity;
    return 0;
}

/*
 * Read one line into `line`.  Returns 1 for a line, 0 at end of file and -1
 * when the buffer could not grow.
 *
 * A line longer than MESSAGE_TABLE_MAX_LINE is not stored: the rest of it is
 * consumed so the next line starts where it should, *too_long is set, and the
 * caller skips it as a whole.  Truncating it instead would hand the splitter a
 * line that looks complete and quietly replace a message with the first half of
 * a replacement nobody wrote.
 */
static int
table_read_line(TableReader *reader, TableLine *line, int *too_long)
{
    *too_long = 0;
    line->length = 0;
    int any = 0;

    for (;;) {
        int ch = table_reader_next(reader);
        if (ch < 0) {
            break;                  /* end of file */
        }
        any = 1;
        if (ch == '\n') {
            break;
        }
        if (line->length >= MESSAGE_TABLE_MAX_LINE) {
            *too_long = 1;          /* swallow the rest of this line */
            continue;
        }
        if (table_line_reserve(line, line->length + 2) < 0) {
            return -1;
        }
        line->data[line->length++] = (char)ch;
    }

    if (!any) {
        return 0;
    }
    if (table_line_reserve(line, line->length + 1) < 0) {
        return -1;
    }
    line->data[line->length] = '\0';
    return 1;
}

/*
 * Read the whole table from an open file into a fresh MessageTable.
 */
static MessageTable *
message_table_read(FILE *fp)
{
    MessageTable *table = PyMem_Calloc(1, sizeof(MessageTable));
    if (table == NULL) {
        PyErr_NoMemory();
        return NULL;
    }
    table->literals = PyDict_New();
    if (table->literals == NULL) {
        PyMem_Free(table);
        return NULL;
    }

    TableReader reader;
    table_reader_init(&reader, fp);
    TableLine line;
    if (table_line_init(&line) < 0) {
        Py_DECREF(table->literals);
        PyMem_Free(table);
        return NULL;
    }

    int too_long = 0;
    int status = 0;
    while ((status = table_read_line(&reader, &line, &too_long)) > 0) {
        if (too_long) {
            table->lines_too_long++;
            continue;
        }
        char *key = NULL;
        char *hex = NULL;
        int verdict = table_split_line(line.data, line.length, &key, &hex);
        if (verdict == 0) {
            continue;               /* blank or a comment */
        }
        if (verdict < 0) {
            table->lines_skipped++;
            PyErr_Clear();
            continue;
        }
        if (table->entry_count >= MESSAGE_TABLE_MAX_ENTRIES) {
            table->lines_skipped++;
            continue;
        }
        message_table_add(table, key, hex);
    }
    if (status < 0) {
        /* Out of memory while reading a line: keep what was read. */
        PyErr_Clear();
    }

    table_line_free(&line);
    return table;
}

/*
 * Build a table from the first candidate that can be read.
 *
 * Returns a new capsule holding the table, or NULL when no candidate holds a
 * readable table (which is a normal configuration) or when reading one failed.
 * Nothing is remembered on failure: a table that appears later is picked up by
 * the next lookup.
 */
static PyObject *
message_table_build(void)
{
    for (int source = 0; source < (int)MESSAGE_TABLE_SOURCE_COUNT; source++) {
        if (!message_table_candidate_wanted((MessageTableSource)source)) {
            PyErr_Clear();
            continue;
        }
        PyObject *path = message_table_candidate((MessageTableSource)source);
        if (path == NULL) {
            PyErr_Clear();
            continue;
        }
        FILE *fp = message_table_open(path);
        if (fp == NULL) {
            PyErr_Clear();
            Py_DECREF(path);
            continue;               /* not readable: try the next candidate */
        }
        MessageTable *table = NULL;
        if (message_table_size_ok(fp)) {
            table = message_table_read(fp);
        }
        fclose(fp);
        Py_DECREF(path);
        if (table == NULL) {
            PyErr_Clear();
            continue;
        }
        if (table->lines_skipped > 0 || table->lines_too_long > 0) {
            /* Say so once, and only where a diagnostic is expected anyway: a
               silently dropped entry is the one failure mode that is otherwise
               impossible to see from the outside. */
            if (message_table_sys_flag("dev_mode")) {
                PySys_WriteStderr(
                    "message table: %zd malformed line(s) and %zd line(s) over "
                    "%d bytes ignored\n",
                    table->lines_skipped, table->lines_too_long,
                    (int)MESSAGE_TABLE_MAX_LINE);
            }
            PyErr_Clear();
        }
        PyObject *capsule = PyCapsule_New(table, MESSAGE_TABLE_CAPSULE_NAME,
                                          message_table_capsule_free);
        if (capsule == NULL) {
            PyErr_Clear();
            message_table_free(table);
            continue;
        }
        return capsule;
    }
    PyErr_Clear();
    return NULL;
}

/*
 * The table of the current interpreter, as a capsule the caller has to keep
 * alive for as long as it uses the table.
 *
 * Returns a new reference, or NULL when there is no table to use.  Building it
 * twice under a race costs one wasted table: the two threads both publish with
 * PyDict_SetDefaultRef(), one wins, and the loser drops its own copy.
 */
static PyObject *
message_table_acquire(void)
{
    PyObject *dict = message_table_interp_dict();
    if (dict == NULL) {
        PyErr_Clear();
        return NULL;
    }

    PyObject *capsule = NULL;
    if (PyDict_GetItemStringRef(dict, MESSAGE_TABLE_STATE_KEY, &capsule) < 0) {
        PyErr_Clear();
        return NULL;
    }
    if (capsule != NULL) {
        return capsule;
    }

    PyObject *fresh = message_table_build();
    if (fresh == NULL) {
        return NULL;
    }
    PyObject *key = PyUnicode_FromString(MESSAGE_TABLE_STATE_KEY);
    if (key == NULL) {
        PyErr_Clear();
        Py_DECREF(fresh);
        return NULL;
    }
    PyObject *published = NULL;
    if (PyDict_SetDefaultRef(dict, key, fresh, &published) < 0) {
        PyErr_Clear();
        Py_DECREF(key);
        Py_DECREF(fresh);
        return NULL;
    }
    Py_DECREF(key);
    Py_DECREF(fresh);               /* the dict holds it now */
    return published;
}

/* Drop this interpreter's table and proxy cache. */
void
_PyMessageTable_Fini(void)
{
    PyObject *dict = message_table_interp_dict();
    if (dict == NULL) {
        PyErr_Clear();
        return;
    }
    if (PyDict_DelItemString(dict, MESSAGE_TABLE_STATE_KEY) < 0) {
        PyErr_Clear();
    }
    if (PyDict_DelItemString(dict, MESSAGE_TABLE_PROXIES_KEY) < 0) {
        PyErr_Clear();
    }
}

/* ------------------------------------------------------------------ */
/* which messages can be replaced                                      */
/* ------------------------------------------------------------------ */

/*
 * The message `exc` would display, or NULL when there is nothing to match.
 *
 * Two cases, in this order:
 *
 *   1. When tp_str is BaseException's, the displayed text is defined to be
 *      str(args[0]), so a str args[0] is the text itself and no string has to
 *      be built.  This is the fast path, and it is also the only case the
 *      table could ever match before it learned about patterns.  (With more
 *      than one argument the display is the repr of the whole tuple; the fast
 *      path still takes args[0], which is what the table is written against.)
 *
 *   2. Otherwise the display text is whatever __str__ computes: OSError
 *      prepends "[Errno 2] ", KeyError applies repr(), UnicodeDecodeError
 *      builds its own line.  That text is used as it is -- there is no
 *      assumption that it starts with args[0] (which used to put all of those
 *      out of reach), and args does not have to hold a string at all.
 *
 * Returns a new reference, or NULL with no exception set.
 */
static PyObject *
replaceable_message(PyObject *exc)
{
    if (Py_TYPE(exc)->tp_str == Py_TYPE(PyExc_BaseException)->tp_str) {
        PyObject *args = PyException_GetArgs(exc);
        if (args == NULL) {
            PyErr_Clear();
        }
        else {
            if (PyTuple_Check(args) && PyTuple_GET_SIZE(args) > 0) {
                PyObject *first = PyTuple_GET_ITEM(args, 0);
                if (PyUnicode_Check(first)) {
                    Py_DECREF(args);
                    return Py_NewRef(first);
                }
            }
            Py_DECREF(args);
        }
    }

    PyObject *shown = PyObject_Str(exc);
    if (shown == NULL) {
        PyErr_Clear();
    }
    return shown;
}

/*
 * A shallow copy of `exc`, created as an instance of `type`: same args, same
 * instance dict, and the same traceback, cause and context (those live in
 * C-level slots, not in __dict__).  The copy is what carries the replacement,
 * so the exception the user still holds is never touched.  `type` is normally
 * Py_TYPE(exc); the display proxy below passes a subclass of it instead.
 *
 * Returns a new reference, or NULL.
 */
static PyObject *
copy_exception_as(PyObject *exc, PyObject *type)
{
    PyObject *copy = PyObject_CallMethod(type, "__new__", "O", type);
    if (copy == NULL) {
        PyErr_Clear();
        return NULL;
    }

    PyObject *state = PyObject_GetAttrString(exc, "__dict__");
    if (state != NULL) {
        PyObject *target = PyObject_GetAttrString(copy, "__dict__");
        if (target != NULL) {
            if (PyDict_Update(target, state) < 0) {
                PyErr_Clear();
            }
            Py_DECREF(target);
        }
        else {
            PyErr_Clear();
        }
        Py_DECREF(state);
    }
    else {
        PyErr_Clear();
    }

    PyObject *args = PyException_GetArgs(exc);
    if (args != NULL) {
        if (PyObject_SetAttrString(copy, "args", args) < 0) {
            PyErr_Clear();
        }
        Py_DECREF(args);
    }
    else {
        PyErr_Clear();
    }

    PyObject *tb = PyException_GetTraceback(exc);
    if (tb != NULL) {
        if (PyException_SetTraceback(copy, tb) < 0) {
            PyErr_Clear();
        }
        Py_DECREF(tb);
    }
    else {
        PyErr_Clear();
    }

    PyObject *cause = PyException_GetCause(exc);
    if (cause != NULL) {
        PyException_SetCause(copy, cause);      /* steals the reference */
    }
    else {
        PyErr_Clear();
    }

    PyObject *context = PyException_GetContext(exc);
    if (context != NULL) {
        PyException_SetContext(copy, context);  /* steals the reference */
    }
    else {
        PyErr_Clear();
    }

    PyObject *suppress = PyObject_GetAttrString(exc, "__suppress_context__");
    if (suppress != NULL) {
        if (PyObject_SetAttrString(copy, "__suppress_context__",
                                   suppress) < 0) {
            PyErr_Clear();
        }
        Py_DECREF(suppress);
    }
    else {
        PyErr_Clear();
    }

    return copy;
}

/* A copy of the same type as `exc`; see copy_exception_as(). */
static PyObject *
copy_exception(PyObject *exc)
{
    return copy_exception_as(exc, (PyObject *)Py_TYPE(exc));
}

/*
 * The fields a SyntaxError renders its location from: the message plus the
 * file, the line and the span the caret covers.
 */
static const char *const syntax_error_location[] = {
    "filename", "lineno", "offset", "text", "end_lineno", "end_offset", NULL,
};

/*
 * Put `replacement` into the message of a SyntaxError `copy` and carry the
 * location over from `value`.
 *
 * Both live in C-level members, so a copy built with __new__ has them NULL and
 * copying __dict__ does not bring them along: without this the copy would
 * display the bare message and the traceback module would print no file, no
 * line and no caret at all.  Returns 0 when any of them cannot be carried
 * over, which leaves the caller to print the original message.
 */
static int
set_syntax_error_message(PyObject *value, PyObject *copy,
                         PyObject *replacement)
{
    if (PyObject_SetAttrString(copy, "msg", replacement) < 0) {
        PyErr_Clear();
        return 0;
    }
    for (const char *const *name = syntax_error_location;
         *name != NULL; name++) {
        PyObject *field = PyObject_GetAttrString(value, *name);
        if (field == NULL) {
            PyErr_Clear();
            return 0;
        }
        if (field != Py_None
            && PyObject_SetAttrString(copy, *name, field) < 0) {
            PyErr_Clear();
            Py_DECREF(field);
            return 0;
        }
        Py_DECREF(field);
    }
    return 1;
}

/*
 * Does `copy` display the replacement?
 *
 * exception_with_message() writes the new text into args[0], and that only
 * reaches the display when the type renders args[0].  A type that builds its
 * text some other way shows something else instead: UnicodeDecodeError shows
 * the empty string (its __str__ needs the `object` member, which a copy built
 * with __new__ never gets), an OSError carrying errno shows the repr of its
 * whole args tuple, KeyError shows repr() of the message, and a type with its
 * own __str__ shows its own fixed text.  Printing any of those would lose the
 * message the user needs or wrap the replacement in quotes, so the rewrite is
 * accepted only when the copy displays exactly the replacement.
 *
 * SyntaxError is the one type that appends something of its own, the location
 * ("<msg> (<file>, line N)"), so there the display has to start with the new
 * message.
 *
 * Returns 1 when the display is right, 0 when the rewrite has to be dropped.
 */
static int
copy_shows_replacement(PyObject *value, PyObject *copy, PyObject *replacement)
{
    PyObject *shown = PyObject_Str(copy);
    if (shown == NULL) {
        PyErr_Clear();
        return 0;
    }

    int ok = 0;
    if (PyUnicode_Check(shown)) {
        int cmp = PyUnicode_Compare(shown, replacement);
        if (cmp == 0) {
            ok = 1;
        }
        else if (PyErr_Occurred()) {
            PyErr_Clear();          /* the comparison failed: no rewrite */
        }
        else if (PyObject_TypeCheck(value,
                                    (PyTypeObject *)PyExc_SyntaxError)) {
            Py_ssize_t at = PyUnicode_Find(shown, replacement, 0,
                                           PY_SSIZE_T_MAX, 1);
            if (at == 0) {
                ok = 1;             /* the message, then the location */
            }
            else if (at < 0) {
                PyErr_Clear();      /* not found, or -2 with an error set */
            }
        }
    }
    else {
        PyErr_Clear();
    }
    Py_DECREF(shown);
    return ok;
}

/* ------------------------------------------------------------------ */
/* display proxy                                                       */
/* ------------------------------------------------------------------ */

/*
 * A few types build their displayed text from C-level members instead of
 * args[0], so a replacement written there never reaches the display:
 *
 *     FileNotFoundError: [Errno 2] No such file or directory: 'no-such-file'
 *     UnicodeDecodeError: 'utf-8' codec can't decode byte 0xff ...
 *     KeyError: 'no-such-key'
 *
 * For those the replacement is carried by a display proxy: a copy whose type
 * is a subclass of the original one, with a __str__ that returns the
 * replacement.  A subclass, rather than a plain exception, keeps the printed
 * name ("FileNotFoundError: ..." comes from __qualname__ and __module__ in
 * traceback, from tp_name in the C fallback) and keeps isinstance() checks
 * working on the object that is displayed.
 *
 * The subclass is created once per base type and cached, because the same
 * type shows up in every traceback.
 */
#define MESSAGE_TABLE_DISPLAY_ATTR "_PyMessageTable_display"

/*
 * Upper bound on the cached proxy classes.  One proxy is needed per exception
 * type that is displayed, and a program is free to build exception classes at
 * run time (a factory that calls type(), a plugin system), so the cache gets a
 * ceiling: past it a proxy is still built and used, it is just not kept.
 *
 * The cache is a list of (weak reference to base, weak reference to proxy)
 * pairs living in the interpreter's dict.  Weak on purpose: a proxy is a
 * subclass of the base, so a strong entry would keep every exception class that
 * was ever displayed alive until the interpreter ends.  With weak references a
 * class built at run time is collected as soon as the program drops it, and the
 * dead pair is simply skipped (and eventually outnumbered) by later lookups.
 */
#define MESSAGE_PROXY_MAX_CACHED 64

/*
 * __str__ of a display proxy: the replacement the copy carries, or the empty
 * string when the attribute is missing.  It never falls back to str(self),
 * which would call this function again.
 */
static PyObject *
display_proxy_str(PyObject *self, PyObject *ignored)
{
    (void)ignored;
    PyObject *text = PyObject_GetAttrString(self, MESSAGE_TABLE_DISPLAY_ATTR);
    if (text == NULL) {
        PyErr_Clear();
        text = PyUnicode_FromString("");
    }
    return text;
}

/* Handed to PyDescr_NewMethod() below; the entry has to outlive the class. */
static PyMethodDef display_proxy_str_method = {
    "__str__", display_proxy_str, METH_NOARGS, NULL,
};

/* The current interpreter's proxy list, created on first use. */
static PyObject *
display_proxies_acquire(void)
{
    PyObject *dict = message_table_interp_dict();
    if (dict == NULL) {
        PyErr_Clear();
        return NULL;
    }
    PyObject *list = NULL;
    if (PyDict_GetItemStringRef(dict, MESSAGE_TABLE_PROXIES_KEY, &list) < 0) {
        PyErr_Clear();
        return NULL;
    }
    if (list != NULL) {
        return list;
    }
    PyObject *fresh = PyList_New(0);
    if (fresh == NULL) {
        PyErr_Clear();
        return NULL;
    }
    PyObject *key = PyUnicode_FromString(MESSAGE_TABLE_PROXIES_KEY);
    if (key == NULL) {
        PyErr_Clear();
        Py_DECREF(fresh);
        return NULL;
    }
    PyObject *published = NULL;
    if (PyDict_SetDefaultRef(dict, key, fresh, &published) < 0) {
        PyErr_Clear();
        Py_DECREF(key);
        Py_DECREF(fresh);
        return NULL;
    }
    Py_DECREF(key);
    Py_DECREF(fresh);               /* the dict holds it now */
    return published;
}

/* The cached proxy for `base`, or NULL.  Returns a new reference. */
static PyObject *
display_proxy_cached(PyObject *list, PyObject *base)
{
    Py_ssize_t size = PyList_GET_SIZE(list);
    for (Py_ssize_t i = 0; i < size; i++) {
        PyObject *item = PyList_GetItemRef(list, i);
        if (item == NULL) {
            PyErr_Clear();
            return NULL;
        }
        if (!PyTuple_Check(item) || PyTuple_GET_SIZE(item) != 2) {
            Py_DECREF(item);
            continue;
        }
        PyObject *cached_base = NULL;
        if (PyWeakref_GetRef(PyTuple_GET_ITEM(item, 0), &cached_base) == 1) {
            if (cached_base == base) {
                PyObject *proxy = NULL;
                if (PyWeakref_GetRef(PyTuple_GET_ITEM(item, 1), &proxy) == 1) {
                    Py_DECREF(cached_base);
                    Py_DECREF(item);
                    return proxy;
                }
                PyErr_Clear();
            }
            Py_XDECREF(cached_base);
        }
        else {
            PyErr_Clear();          /* the base is gone: a dead entry */
        }
        Py_DECREF(item);
    }
    return NULL;
}

/* Remember `proxy` as the proxy of `base`, unless the cache is full. */
static void
display_proxy_remember(PyObject *list, PyObject *base, PyObject *proxy)
{
    if (PyList_GET_SIZE(list) >= MESSAGE_PROXY_MAX_CACHED) {
        return;
    }
    PyObject *base_ref = PyWeakref_NewRef(base, NULL);
    PyObject *proxy_ref = PyWeakref_NewRef(proxy, NULL);
    PyObject *item = (base_ref != NULL && proxy_ref != NULL)
        ? PyTuple_Pack(2, base_ref, proxy_ref) : NULL;
    if (item == NULL) {
        PyErr_Clear();
    }
    else if (PyList_Append(list, item) < 0) {
        /* Two threads can build the same proxy at once; the extra pair is
           harmless, the lookup takes the first live one. */
        PyErr_Clear();
    }
    Py_XDECREF(item);
    Py_XDECREF(base_ref);
    Py_XDECREF(proxy_ref);
}

/*
 * The proxy class for `base`, or NULL with an exception set.
 *
 * The name, the qualified name and the module are copies of the base's, so
 * that the line traceback prints for a proxy is the line it would print for
 * the original exception.  Failing to create the class only costs the
 * rewrite: the caller keeps the original message.
 */
static PyObject *
display_proxy_class(PyObject *base)
{
    PyObject *list = display_proxies_acquire();
    if (list != NULL) {
        PyObject *cached = display_proxy_cached(list, base);
        if (cached != NULL) {
            Py_DECREF(list);
            return cached;
        }
    }
    else {
        PyErr_Clear();              /* no interpreter dict: build, do not keep */
    }

    PyObject *name = PyObject_GetAttrString(base, "__name__");
    PyObject *qualname = PyObject_GetAttrString(base, "__qualname__");
    PyObject *module = PyObject_GetAttrString(base, "__module__");
    PyObject *bases = PyTuple_Pack(1, base);
    PyObject *namespace = PyDict_New();
    PyObject *method = NULL;
    PyObject *proxy = NULL;

    if (name == NULL || qualname == NULL || module == NULL || bases == NULL
        || namespace == NULL) {
        goto done;
    }
    if (PyDict_SetItemString(namespace, "__qualname__", qualname) < 0
        || PyDict_SetItemString(namespace, "__module__", module) < 0) {
        goto done;
    }
    /* The descriptor belongs to the base, not to the subclass: binding it
       only asks that the instance be an instance of that base, which every
       proxy instance is. */
    method = PyDescr_NewMethod((PyTypeObject *)base,
                               &display_proxy_str_method);
    if (method == NULL
        || PyDict_SetItemString(namespace, "__str__", method) < 0) {
        goto done;
    }
    proxy = PyObject_CallFunction((PyObject *)&PyType_Type, "OOO",
                                  name, bases, namespace);
    if (proxy == NULL) {
        goto done;
    }

    /* Remember it.  Running out of memory, or reaching the ceiling, is not
       worth failing over: the proxy is usable as it is, it just will not be
       reused. */
    if (list != NULL) {
        display_proxy_remember(list, base, proxy);
    }

done:
    Py_XDECREF(list);
    Py_XDECREF(name);
    Py_XDECREF(qualname);
    Py_XDECREF(module);
    Py_XDECREF(bases);
    Py_XDECREF(namespace);
    Py_XDECREF(method);
    return proxy;
}

/*
 * Is the text the table matched the whole text `value` displays?
 *
 * When tp_str is BaseException's, the lookup is done on args[0], while the
 * display is the repr of the whole args tuple as soon as there is more than
 * one argument.  Replacing the display with the table's text would then drop
 * the arguments the key never covered, so those instances are left alone.
 * Every other type displays exactly the text the lookup ran on.
 */
static int
message_is_whole_display(PyObject *value)
{
    if (Py_TYPE(value)->tp_str != Py_TYPE(PyExc_BaseException)->tp_str) {
        return 1;
    }
    PyObject *args = PyException_GetArgs(value);
    if (args == NULL) {
        PyErr_Clear();
        return 0;
    }
    int whole = PyTuple_Check(args) && PyTuple_GET_SIZE(args) <= 1;
    Py_DECREF(args);
    return whole;
}

/*
 * A copy of `value` that displays `replacement` whatever its own __str__ would
 * say.  Returns a new reference, or NULL when no proxy can be built or the
 * copy would not display the replacement.
 */
static PyObject *
exception_with_display(PyObject *value, PyObject *replacement)
{
    PyObject *proxy_type = display_proxy_class((PyObject *)Py_TYPE(value));
    if (proxy_type == NULL) {
        PyErr_Clear();
        return NULL;
    }
    PyObject *copy = copy_exception_as(value, proxy_type);
    Py_DECREF(proxy_type);
    if (copy == NULL) {
        return NULL;
    }
    if (PyObject_SetAttrString(copy, MESSAGE_TABLE_DISPLAY_ATTR,
                               replacement) < 0) {
        PyErr_Clear();
        Py_DECREF(copy);
        return NULL;
    }
    if (!copy_shows_replacement(value, copy, replacement)) {
        Py_DECREF(copy);
        return NULL;
    }
    return copy;
}

/*
 * Show `replacement` instead of the message `value` carries.
 *
 * Returns a NEW reference to the rewritten copy, or NULL when this exception
 * cannot be rewritten, either because the copy cannot be built or because it
 * would not display the replacement.
 *
 * Which route is taken depends on where the displayed text comes from:
 *
 *   - tp_str is BaseException's, so the display is args[0] (or the repr of the
 *     whole argument tuple when there is more than one argument, which is why
 *     that case is left alone).  Writing the replacement into args[0] is enough,
 *     and the copy is checked to display exactly the replacement before it is
 *     accepted.
 *   - tp_str is the type's own, so the display is whatever that __str__
 *     computes.  Two things follow.  The equality check cannot be the gate: a
 *     __str__ that answers differently on every call would pass the check and
 *     then print something else the table never asked for, so the check is not
 *     what decides here.  And the replacement cannot be carried by args[0],
 *     because that is not what gets displayed: a display proxy carries it.
 *     SyntaxError is the exception to both: it appends its location, so its own
 *     path is used and the display only has to start with the new message.
 */
static PyObject *
exception_with_message(PyObject *value, PyObject *replacement)
{
    int custom_str = Py_TYPE(value)->tp_str
        != Py_TYPE(PyExc_BaseException)->tp_str;
    int syntax_error = PyObject_TypeCheck(value,
                                          (PyTypeObject *)PyExc_SyntaxError);

    if (custom_str && !syntax_error) {
        if (!message_is_whole_display(value)) {
            return NULL;
        }
        return exception_with_display(value, replacement);
    }

    PyObject *copy = copy_exception(value);
    if (copy == NULL) {
        return NULL;
    }

    PyObject *args = PyException_GetArgs(copy);
    if (args == NULL || !PyTuple_Check(args)) {
        PyErr_Clear();
        Py_XDECREF(args);
        Py_DECREF(copy);
        return NULL;
    }

    Py_ssize_t argc = PyTuple_GET_SIZE(args);
    PyObject *newargs = PyTuple_New(argc > 0 ? argc : 1);
    if (newargs == NULL) {
        Py_DECREF(args);
        Py_DECREF(copy);
        return NULL;
    }
    PyTuple_SET_ITEM(newargs, 0, Py_NewRef(replacement));
    for (Py_ssize_t i = 1; i < argc; i++) {
        PyTuple_SET_ITEM(newargs, i,
                         Py_NewRef(PyTuple_GET_ITEM(args, i)));
    }
    Py_DECREF(args);

    if (PyObject_SetAttrString(copy, "args", newargs) < 0) {
        PyErr_Clear();
        Py_DECREF(newargs);
        Py_DECREF(copy);
        return NULL;
    }
    Py_DECREF(newargs);

    if (syntax_error && !set_syntax_error_message(value, copy, replacement)) {
        Py_DECREF(copy);
        return NULL;
    }

    if (!copy_shows_replacement(value, copy, replacement)) {
        Py_DECREF(copy);
        return NULL;
    }
    return copy;
}

/* ------------------------------------------------------------------ */
/* lookup                                                              */
/* ------------------------------------------------------------------ */

/*
 * The replacement of a literal entry whose key equals `text`, or NULL.
 *
 * The literals live in a dict, so this is one hashed lookup instead of a scan
 * over every literal entry of the table; the comparison itself is still str
 * equality, which is what a literal key has always meant.
 */
static PyObject *
literal_replacement(MessageTable *table, PyObject *text)
{
    PyObject *value = NULL;
    if (PyDict_GetItemRef(table->literals, text, &value) < 0) {
        PyErr_Clear();
        return NULL;
    }
    return value;                   /* new reference, or NULL for a miss */
}

/*
 * The text of the pattern entry that applies to `text`, or NULL.
 *
 * Patterns are tried only after every literal key has failed, and the winner
 * is the matching pattern with the fewest wildcards; an equal count is decided
 * by position in the table, which is the order this scan visits entries in.
 *
 * Every pattern of a lookup shares one budget, so a table full of patterns that
 * backtrack cannot add up to minutes of work for a single traceback.
 */
static PyObject *
pattern_replacement(MessageTable *table, PyObject *text)
{
    if (table->pattern_count == 0) {
        /* A table of literal keys alone: nothing to do, and no reason to build
           the UTF-8 form of a text that no pattern will be matched against. */
        return NULL;
    }
    Py_ssize_t text_size = 0;
    const char *text_utf8 = PyUnicode_AsUTF8AndSize(text, &text_size);
    if (text_utf8 == NULL) {
        /* A str holding a lone surrogate has no UTF-8 form, so there are no
           bytes to match against: keep the message. */
        PyErr_Clear();
        return NULL;
    }

    size_t budget = MESSAGE_PATTERN_CALL_BUDGET;
    MessageEntry *winner = NULL;
    size_t fewest = (size_t)-1;
    for (Py_ssize_t i = 0; i < table->pattern_count; i++) {
        MessageEntry *entry = &table->patterns[i];
        if (entry->star_count >= fewest) {
            continue;               /* cannot beat the pattern that matched */
        }
        if (!pattern_match(entry->key_utf8, (size_t)entry->key_utf8_size,
                           text_utf8, (size_t)text_size,
                           entry->star_count, &budget, NULL)) {
            continue;
        }
        winner = entry;
        fewest = entry->star_count;
    }
    if (winner == NULL) {
        return NULL;
    }

    /* Match the winner again, now collecting the runs: the scan above cannot
       keep them, because a closer match would overwrite the buffer. */
    size_t captures[2 * MESSAGE_PATTERN_MAX_STARS];
    if (!pattern_match(winner->key_utf8, (size_t)winner->key_utf8_size,
                       text_utf8, (size_t)text_size, winner->star_count,
                       NULL, captures)) {
        return NULL;
    }

    char *expanded = NULL;
    size_t expanded_size = 0;
    if (!pattern_expand(winner->value_utf8, (size_t)winner->value_utf8_size,
                        text_utf8, captures, winner->star_count,
                        &expanded, &expanded_size)) {
        /* The replacement names a capture the pattern does not have: keep the
           original message instead of inventing text. */
        return NULL;
    }
    /* The expansion is a slice of a table line that was decoded as UTF-8 before
       it was hex encoded, so it decodes strictly here as well; anything else
       would be a bug in the engine, and dropping the rewrite is the safe end of
       that. */
    PyObject *result = PyUnicode_DecodeUTF8(expanded,
                                            (Py_ssize_t)expanded_size,
                                            "strict");
    free(expanded);
    if (result == NULL) {
        PyErr_Clear();
    }
    return result;
}

/* ------------------------------------------------------------------ */
/* the hook                                                            */
/* ------------------------------------------------------------------ */

/*
 * Look the exception up in the table.
 *
 * Returns a NEW reference to a copy of `value` carrying the replacement
 * message, or NULL when nothing was replaced.  This replaces the 3.10 entry
 * point, which took the type/value/tb triple.
 *
 * The error indicator is saved on the way in and restored on the way out.  This
 * runs while an exception is being displayed and the caller owns whatever the
 * indicator holds, so a lookup that failed on the way -- an unreadable table, a
 * message without a UTF-8 form, a class that refuses to be subclassed -- must
 * neither report its own failure nor destroy the caller's pending exception.
 */
PyObject *
_PyErr_Message_Replace(PyObject *value)
{
    if (value == NULL || !PyExceptionInstance_Check(value)) {
        return NULL;
    }

    PyObject *saved = PyErr_GetRaisedException();   /* clears the indicator */
    PyObject *result = NULL;
    PyObject *capsule = message_table_acquire();
    MessageTable *table = NULL;
    if (capsule != NULL) {
        table = PyCapsule_GetPointer(capsule, MESSAGE_TABLE_CAPSULE_NAME);
        if (table == NULL) {
            PyErr_Clear();
        }
    }

    if (table != NULL) {
        PyObject *text = replaceable_message(value);
        if (text != NULL) {
            PyObject *replacement = literal_replacement(table, text);
            if (replacement == NULL) {
                replacement = pattern_replacement(table, text);
            }
            Py_DECREF(text);
            if (replacement != NULL) {
                result = exception_with_message(value, replacement);
                Py_DECREF(replacement);
            }
        }
    }
    Py_XDECREF(capsule);
    if (result == NULL) {
        PyErr_Clear();
    }
    PyErr_SetRaisedException(saved);    /* steals; NULL means "none" */
    return result;
}

/* Reload the table from disk; returns the entry count.  For tests. */
Py_ssize_t
_PyMessageTable_Reload(void)
{
    _PyMessageTable_Fini();
    PyObject *capsule = message_table_acquire();
    if (capsule == NULL) {
        PyErr_Clear();
        return 0;
    }
    MessageTable *table = PyCapsule_GetPointer(capsule,
                                               MESSAGE_TABLE_CAPSULE_NAME);
    if (table == NULL) {
        PyErr_Clear();
        Py_DECREF(capsule);
        return 0;
    }
    Py_ssize_t count = table->entry_count;
    Py_DECREF(capsule);
    return count;
}

#endif /* !MESSAGE_TABLE_NO_PYTHON */
