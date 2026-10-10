/*
 * Message replacement table — public declarations for CPython 3.11+.
 *
 * Included from Include/cpython/unicodeobject.h (see attach.patch), which is a
 * pragmatic home for it: that header is pulled in by Python.h and already
 * carries the other Unicode-facing C API.  Moving it to Include/internal/ is
 * the cleaner long-term option if this ever leaves a private fork.
 */

#ifndef Py_MESSAGE_TABLE_H
#define Py_MESSAGE_TABLE_H
#ifdef __cplusplus
extern "C" {
#endif

/*
 * Prepare a display copy of `value` carrying the replacement message, when the
 * table has an entry for the message it currently shows.
 *
 * `value` is an exception instance.  Returns a NEW reference to a copy of it
 * with the message rewritten, or NULL when nothing was replaced — either
 * because the table has no matching entry, or because this exception type does
 * not derive its displayed message from args[0] (OSError, UnicodeDecodeError,
 * KeyError, ...).  Callers must treat NULL as "use the original".
 *
 * Implemented in Objects/message_table.c.
 */
PyAPI_FUNC(PyObject *) _PyErr_Message_Replace(PyObject *value);

/*
 * Decode a hex string (UTF-8 bytes, as produced by bytes.hex()) into a str.
 * Returns NULL with an exception set for NULL input, an odd length, or a
 * non-hex character.
 */
PyAPI_FUNC(PyObject *) PyUnicode_FromHex(const char *hex);

/* Reload the cached table from disk.  Returns the number of entries loaded
   (0 when no table is configured or readable).  Intended for tests. */
PyAPI_FUNC(Py_ssize_t) _PyMessageTable_Reload(void);

/* Release the cached table.  Called from _Py_Finalize(). */
PyAPI_FUNC(void) _PyMessageTable_Fini(void);

#ifdef __cplusplus
}
#endif
#endif /* !Py_MESSAGE_TABLE_H */
