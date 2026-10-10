/*
 * Message replacement table for CPython 3.11+ (developed against 3.15).
 *
 * Ported from a 3.10 fork that replaced exception messages at print time.  Two
 * upstream changes shape this version:
 *
 *   1. 3.11 replaced the type/value/traceback triple with a single exception
 *      instance, so the hook no longer rebuilds a value tuple.
 *   2. Not every exception derives its displayed text from args[0].  OSError,
 *      UnicodeDecodeError, KeyError and types with a custom __str__ build the
 *      message from dedicated fields, and rewriting args there would silently
 *      change nothing.  Those instances are detected and skipped.
 *
 * Everything is plain C on public API: nothing here imports Python code, so a
 * traceback stays printable no matter how broken the interpreter's import
 * machinery happens to be at that moment.
 *
 * Table format (UTF-8, one entry per line, blank lines and '#' comments
 * ignored):
 *
 *     # original message,hex-encoded UTF-8 replacement
 *     division by zero,e7aca8e89b8be6898de4bc9ae68a8a30e694bee588b0e999a4e695b0e590a721
 *
 * The right-hand side is what Python's bytes.hex() produces:
 *
 *     print(input().encode('utf-8').hex())
 */

#if defined(MESSAGE_TABLE_STUB_BUILD)
/* Syntax/functional test build: the stub stands in for the CPython headers. */
#include "py_stub_api.h"
#else
#include "Python.h"
#endif

#include <errno.h>
#include <string.h>

/* ------------------------------------------------------------------ */
/* table                                                               */
/* ------------------------------------------------------------------ */

typedef struct {
    PyObject *key;              /* original message, a str object */
    PyObject *value;            /* replacement text, a str object */
} MessageEntry;

typedef struct {
    MessageEntry *entries;
    Py_ssize_t count;
} MessageTable;

#define MESSAGE_TABLE_ENV "PYTHON_MESSAGE_TABLE"
#define MESSAGE_TABLE_DEFAULT "lookup-table.txt"

static MessageTable *message_table = NULL;   /* NULL: not loaded yet */
static int message_table_failed = 0;         /* 1: load attempted and failed */

static void
message_table_clear(void)
{
    if (message_table == NULL) {
        return;
    }
    for (Py_ssize_t i = 0; i < message_table->count; i++) {
        Py_XDECREF(message_table->entries[i].key);
        Py_XDECREF(message_table->entries[i].value);
    }
    PyMem_Free(message_table->entries);
    PyMem_Free(message_table);
    message_table = NULL;
}

/* Where to look for the table: environment, then $PYTHONHOME, then cwd. */
static PyObject *
message_table_path(void)
{
    const char *env = Py_GETENV(MESSAGE_TABLE_ENV);
    if (env != NULL && env[0] != '\0') {
        return PyUnicode_FromString(env);
    }
    const char *home = Py_GETENV("PYTHONHOME");
    wchar_t *prefix = (home != NULL && home[0] != '\0')
        ? Py_DecodeLocale(home, NULL) : NULL;
    PyObject *path = NULL;
    if (prefix != NULL) {
        path = PyUnicode_FromFormat("%ls/%s", prefix, MESSAGE_TABLE_DEFAULT);
        PyMem_RawFree(prefix);
    }
    if (path == NULL) {
        PyErr_Clear();
        path = PyUnicode_FromString(MESSAGE_TABLE_DEFAULT);
    }
    return path;
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
 * Decode a hex string into a str by way of its UTF-8 bytes.
 *
 * Validates the input: NULL, an odd length or a non-hex character returns NULL
 * with an exception set instead of silently decoding garbage.  (The original
 * version decoded first and only then looked at the length.)
 *
 * File-local on purpose: exporting it would add a public-looking C API that
 * needs documentation, for no gain -- nothing outside this file calls it.
 */
static PyObject *
from_hex(const char *hex)
{
    if (hex == NULL) {
        PyErr_SetString(PyExc_ValueError, "hex string is NULL");
        return NULL;
    }

    size_t len = strlen(hex);
    if (len % 2 != 0) {
        PyErr_Format(PyExc_ValueError, "hex string of odd length: %zu", len);
        return NULL;
    }

    size_t nbytes = len / 2;
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

    PyObject *result = PyUnicode_DecodeUTF8(bytes, (Py_ssize_t)nbytes, "replace");
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
    return utf8 != NULL ? fopen(utf8, "r") : NULL;
#elif defined(MS_WINDOWS)
    wchar_t *wpath = PyUnicode_AsWideCharString(path, NULL);
    if (wpath == NULL) {
        return NULL;
    }
    FILE *fp = _wfopen(wpath, L"r");
    PyMem_Free(wpath);
    return fp;
#else
    PyObject *bytes = PyUnicode_EncodeFSDefault(path);
    if (bytes == NULL) {
        return NULL;
    }
    FILE *fp = fopen(PyBytes_AS_STRING(bytes), "r");
    Py_DECREF(bytes);
    return fp;
#endif
}

static char *
trim_line(char *line)
{
    while (*line == ' ' || *line == '\t' || *line == '\r' || *line == '\n') {
        line++;
    }
    size_t len = strlen(line);
    while (len > 0) {
        char last = line[len - 1];
        if (last != ' ' && last != '\t' && last != '\r' && last != '\n') {
            break;
        }
        line[--len] = '\0';
    }
    return line;
}

#define MESSAGE_TABLE_MAX_LINE 4096

static MessageTable *
message_table_load(void)
{
    PyObject *path = message_table_path();
    if (path == NULL) {
        return NULL;
    }

    FILE *fp = message_table_open(path);
    if (fp == NULL) {
        /* A missing table is a normal configuration, not an error: give up
           quietly and remember it so we do not retry on every exception. */
        PyErr_Clear();
        Py_DECREF(path);
        message_table_failed = 1;
        return NULL;
    }

    Py_ssize_t capacity = 32;
    MessageTable *table = PyMem_Malloc(sizeof(MessageTable));
    if (table == NULL) {
        fclose(fp);
        Py_DECREF(path);
        PyErr_NoMemory();
        return NULL;
    }
    table->entries = PyMem_Malloc((size_t)capacity * sizeof(MessageEntry));
    if (table->entries == NULL) {
        PyMem_Free(table);
        fclose(fp);
        Py_DECREF(path);
        PyErr_NoMemory();
        return NULL;
    }
    table->count = 0;

    char line[MESSAGE_TABLE_MAX_LINE];
    while (fgets(line, (int)sizeof(line), fp) != NULL) {
        char *text = trim_line(line);
        if (text[0] == '\0' || text[0] == '#') {
            continue;
        }
        char *comma = strchr(text, ',');
        if (comma == NULL) {
            continue;                 /* malformed line: skip it */
        }
        *comma = '\0';
        char *key = trim_line(text);
        char *hex = trim_line(comma + 1);
        if (key[0] == '\0' || hex[0] == '\0') {
            continue;
        }

        PyObject *value = from_hex(hex);
        if (value == NULL) {
            /* A malformed value must never stop the interpreter from printing
               the original message: drop the entry and carry on. */
            PyErr_Clear();
            continue;
        }
        PyObject *key_obj = PyUnicode_FromString(key);
        if (key_obj == NULL) {
            Py_DECREF(value);
            PyErr_Clear();
            continue;
        }

        if (table->count == capacity) {
            capacity *= 2;
            MessageEntry *bigger = PyMem_Realloc(
                table->entries, (size_t)capacity * sizeof(MessageEntry));
            if (bigger == NULL) {
                Py_DECREF(key_obj);
                Py_DECREF(value);
                break;                /* keep what we have, drop the rest */
            }
            table->entries = bigger;
        }
        table->entries[table->count].key = key_obj;
        table->entries[table->count].value = value;
        table->count++;
    }

    fclose(fp);
    Py_DECREF(path);
    return table;
}

static MessageTable *
message_table_get(void)
{
    if (message_table != NULL) {
        return message_table;
    }
    if (message_table_failed) {
        return NULL;
    }
    message_table = message_table_load();
    if (message_table == NULL) {
        message_table_failed = 1;
    }
    return message_table;
}

/* Called from _Py_Finalize() (see attach.patch). */
void
_PyMessageTable_Fini(void)
{
    message_table_clear();
    message_table_failed = 0;
}

/* ------------------------------------------------------------------ */
/* which messages can be replaced                                      */
/* ------------------------------------------------------------------ */

/*
 * The message `exc` would display, or NULL when replacing it is impossible.
 *
 * BaseException.__str__ returns str(args[0]) when there is exactly one
 * argument, which covers almost every builtin exception message.  Types that
 * override __str__ are compared against the real display instead: if the text
 * does not start with args[0] (OSError gains an "[Errno 2] " prefix, KeyError
 * adds quotes, UnicodeDecodeError builds its own text), args is not what drives
 * the display and the exception is left alone.
 *
 * Returns a new reference.
 */
static PyObject *
replaceable_message(PyObject *exc)
{
    PyObject *args = PyException_GetArgs(exc);
    if (args == NULL) {
        PyErr_Clear();
        return NULL;
    }
    if (!PyTuple_Check(args) || PyTuple_GET_SIZE(args) == 0) {
        Py_DECREF(args);
        return NULL;
    }

    PyObject *first = PyTuple_GET_ITEM(args, 0);   /* borrowed */
    if (!PyUnicode_Check(first)) {
        Py_DECREF(args);
        return NULL;
    }

    if (Py_TYPE(exc)->tp_str == Py_TYPE(PyExc_BaseException)->tp_str) {
        /* The standard __str__: the display is args[0], no probe needed. */
        PyObject *result = Py_NewRef(first);
        Py_DECREF(args);
        return result;
    }

    Py_ssize_t len = PyUnicode_GET_LENGTH(first);
    PyObject *shown = PyObject_Str(exc);
    Py_DECREF(args);
    if (shown == NULL) {
        PyErr_Clear();
        return NULL;
    }
    int matches = (len > 0 && PyUnicode_GET_LENGTH(shown) >= len
                   && PyUnicode_Tailmatch(shown, first, 0, len, -1) > 0);
    Py_DECREF(shown);
    return matches ? Py_NewRef(first) : NULL;
}

/*
 * A shallow copy of `exc`: same type, same args, same instance dict, and the
 * same traceback, cause and context (those live in C-level slots, not in
 * __dict__).  The copy is what carries the replacement, so the exception the
 * user still holds is never touched.
 *
 * Returns a new reference, or NULL.
 */
static PyObject *
copy_exception(PyObject *exc)
{
    PyTypeObject *type = Py_TYPE(exc);
    PyObject *copy = PyObject_CallMethod((PyObject *)type, "__new__", "O",
                                        (PyObject *)type);
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

/*
 * Show `replacement` instead of the message `value` carries.
 *
 * Returns a NEW reference to the rewritten copy, or NULL when this exception
 * cannot be rewritten.  SyntaxError keeps its message in .msg and its location
 * in args[1], so both fields are updated there.
 */
static PyObject *
exception_with_message(PyObject *value, PyObject *replacement)
{
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

    if (PyObject_TypeCheck(value, (PyTypeObject *)PyExc_SyntaxError)) {
        if (PyObject_SetAttrString(copy, "msg", replacement) < 0) {
            PyErr_Clear();
        }
    }

    return copy;
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
 */
PyObject *
_PyErr_Message_Replace(PyObject *value)
{
    if (value == NULL || !PyExceptionInstance_Check(value)) {
        return NULL;
    }

    MessageTable *table = message_table_get();
    if (table == NULL) {
        return NULL;
    }

    PyObject *text = replaceable_message(value);
    if (text == NULL) {
        return NULL;
    }

    PyObject *replacement = NULL;
    for (Py_ssize_t i = 0; i < table->count; i++) {
        int cmp = PyUnicode_Compare(text, table->entries[i].key);
        if (cmp < 0) {
            PyErr_Clear();            /* comparison failure: no match */
            continue;
        }
        if (cmp == 0) {
            replacement = table->entries[i].value;
            break;
        }
    }
    Py_DECREF(text);

    if (replacement == NULL) {
        return NULL;
    }
    return exception_with_message(value, replacement);
}

/* Reload the table from disk; returns the entry count.  For tests. */
Py_ssize_t
_PyMessageTable_Reload(void)
{
    message_table_clear();
    message_table_failed = 0;
    MessageTable *table = message_table_get();
    return table == NULL ? 0 : table->count;
}
