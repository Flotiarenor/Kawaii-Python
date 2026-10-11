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
 * whose display carries the replacement, or NULL when nothing was replaced:
 * the table has no matching entry, or the rewrite cannot be made to show.
 * A type whose message does not come from args[0] (OSError, UnicodeDecodeError,
 * KeyError, ...) is copied as a subclass with a __str__ that returns the
 * replacement; see the display proxy in Objects/message_table.c.  SyntaxError
 * with more than one argument, and an exception whose display is the repr of a
 * multi-argument tuple, are left alone.  Callers must treat NULL as "use the
 * original".
 *
 * Implemented in Objects/message_table.c.
 */
PyAPI_FUNC(PyObject *) _PyErr_Message_Replace(PyObject *value);

/* Reload the cached table from disk.  Returns the number of entries loaded
   (0 when no table is configured or readable).  Intended for tests. */
PyAPI_FUNC(Py_ssize_t) _PyMessageTable_Reload(void);

/* Release the cached table.  Called from _Py_Finalize(). */
PyAPI_FUNC(void) _PyMessageTable_Fini(void);

#ifdef __cplusplus
}
#endif
#endif /* !Py_MESSAGE_TABLE_H */
