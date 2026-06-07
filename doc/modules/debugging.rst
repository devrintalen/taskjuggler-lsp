Debug logging
=============

The server carries a compile-time, per-category logging facility
(``debug.h`` / ``debug.c``) for tracing its behaviour during
development. This page explains where the output goes, how the
categories and levels work, and how to turn logging on for a build.
For the bare macro and function signatures see the :doc:`../api`
reference (reproduced at the end of this page).

Why stderr
----------

stdout is reserved for the LSP JSON-RPC stream, which the client reads
continuously; writing anything else to it corrupts the protocol. All
debug output therefore goes to **stderr**, which editors typically
surface in a dedicated language-server output channel. ``debug_logf``
takes its own mutex and never touches ``stdout`` or ``stdout_mutex``,
so log lines from the reader, coordinator, query-worker, and
diagnostics-worker threads never interleave with each other or with
protocol traffic.

Categories and levels
---------------------

Each category is an integer *verbosity ceiling*. ``0`` disables the
category; higher values admit progressively more detail. There are
three shared levels:

``LOG_INFO`` (1)
    Key, low-frequency events — one line per operation (a request
    arriving, a document opening, a revalidation completing).

``LOG_VERBOSE`` (2)
    Detailed context — resolved paths, counts, sizes, and timings.

``LOG_TRACE`` (3)
    Very high-frequency detail — per-token, per-job, or per-edge lines.

The categories map onto the server's subsystems:

.. list-table::
   :header-rows: 1
   :widths: 30 70

   * - Category
     - Covers
   * - ``DEBUG_LIFECYCLE``
     - ``initialize`` / ``initialized`` / ``shutdown``, the resolved
       workspace root and ``compile_commands.json`` path, client trace.
   * - ``DEBUG_COMPILE_COMMANDS``
     - ``compile_commands.json`` stat, status transitions, and each
       loaded entry.
   * - ``DEBUG_DOCSTORE``
     - ``didOpen`` / ``didChange`` / ``didClose`` / rename / watched-file
       events and disk loads.
   * - ``DEBUG_INCLUDES``
     - ``include`` resolution, including unresolved or unreadable files.
   * - ``DEBUG_PARSER``
     - the parse pipeline: token, node, and include counts per parse.
   * - ``DEBUG_REVALIDATE``
     - the revalidation cycle, snapshot swap, timing, and the docs[]
       table dump.
   * - ``DEBUG_TJ3``
     - ``tj3`` invocation, exit status and duration, and diagnostic
       path-mapping failures.
   * - ``DEBUG_THREADS``
     - coordinator and worker thread spawn/exit and job-queue activity.
   * - ``DEBUG_RPC``
     - JSON-RPC messages in and out, methods, and payload sizes.

Zero-cost when disabled
-----------------------

Logging is emitted with the ``DLOG`` macro, which expands to a guarded
call::

   #define DLOG(cat, level, ...) \
       do { \
           if ((cat) >= (level)) \
               debug_logf(#cat, (level), __FILE__, __LINE__, __VA_ARGS__); \
       } while (0)

Because ``cat`` expands to a compile-time constant, when a category is
``0`` (or set below the call site's level) the condition is a constant
``false`` and the optimizer removes the whole statement — including the
format string and evaluation of its arguments. A default build (every
category ``0``) therefore carries no logging overhead at all, and the
format string is still type-checked by the compiler.

The category token is also stringified (``#cat``) to tag each line, so
call sites pass the category *macro name* itself::

   DLOG(DEBUG_LIFECYCLE, LOG_INFO,  "initialize: root=%s", root);
   DLOG(DEBUG_PARSER,    LOG_TRACE, "token %d kind=%d", i, kind);

Each emitted line is prefixed with a millisecond timestamp, the thread
id, the category and level, and the source location::

   [20:15:58.836] [tid 139783814772416] [LIFECYCLE/INFO] server.c:1298: initialize: workspace_root=/tmp/x cc_path=/tmp/x/compile_commands.json

Turning logging on
------------------

Raise a category either by editing its default in ``src/debug.h`` or by
overriding it at build time through ``CFLAGS_EXTRA``, which is wired into
both the release (``make``) and debug (``make debug``) builds:

.. code-block:: sh

   make CFLAGS_EXTRA="-DDEBUG_TJ3=3 -DDEBUG_REVALIDATE=2"

Because the levels are compiled in, changing a category requires a
rebuild; there is no runtime switch. The docs[] table dump
(``dump_docs_to_stderr``) is gated behind ``DEBUG_REVALIDATE`` at
``LOG_VERBOSE``, so it is compiled out of the default build instead of
printing on every revalidation.

API reference
-------------

.. doxygenfile:: debug.h
