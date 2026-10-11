/*
 * Message replacement table — public declarations for CPython 3.11+.
 *
 * Included from Include/cpython/unicodeobject.h, which is a pragmatic home for
 * it: that header is pulled in by Python.h and already carries the other
 * Unicode-facing C API.  Moving it to Include/internal/ is the cleaner
 * long-term option if this ever leaves a private fork.
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
 * The error indicator is left exactly as the call found it: the function runs
 * while an exception is being displayed, and the caller's pending exception is
 * none of its business.  In particular it sets no exception of its own when it
 * decides not to replace anything.
 *
 * Implemented in Objects/message_table.c.
 */
PyAPI_FUNC(PyObject *) _PyErr_Message_Replace(PyObject *value);

/* Drop the table and the display-proxy cache of the current interpreter and
   load them again.  Returns the number of entries loaded (0 when no table is
   configured or readable).  Intended for tests. */
PyAPI_FUNC(Py_ssize_t) _PyMessageTable_Reload(void);

/* Drop the table and the display-proxy cache of the current interpreter.  Called
   from _Py_Finalize(); the caches live in the interpreter's own dictionary, so
   this only makes the release point explicit. */
PyAPI_FUNC(void) _PyMessageTable_Fini(void);

#ifdef __cplusplus
}
#endif
#endif /* !Py_MESSAGE_TABLE_H */
