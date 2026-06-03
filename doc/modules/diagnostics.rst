Diagnostics
===========

Diagnostics come from three independent producers — the parser, the
external ``tj3`` scheduler, and the server itself (configuration
warnings such as a missing ``compile_commands.json``) — and arrive
at the editor through one LSP ``textDocument/publishDiagnostics``
notification per URI. Because ``publishDiagnostics`` replaces *all*
markers for a URI in a single message, the producers cannot publish
independently. Their outputs are merged per URI through a small
aggregation type (``diag_set``) before being sent.

Producers
---------

Parser diagnostics
~~~~~~~~~~~~~~~~~~

Syntax errors and parse-time semantic errors are accumulated on the
``ParseOutput`` while the grammar runs and end up attached to the
resulting ``doc_snapshot`` for the document. They are republished by
the coordinator after every parse via ``publish_diagnostics_list()``,
which calls into ``diag_set_publish()`` so the parser's results merge
with whatever the other producers emitted last for the same URI.

tj3 scheduler diagnostics (``src/diag_worker.c``, ``src/tj3.c``)
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

The real TaskJuggler binary (``tj3``) is invoked off the coordinator
to surface the diagnostics it would normally print when scheduling a
project — unresolved references that span files, scheduling errors,
date-range conflicts. Running it inline would stall the server, so
each project gets a long-lived worker thread that:

1. Receives the newest ``workspace_snapshot`` from the coordinator
   (``diag_registry_update()``).
2. Materialises the project's member documents into a temporary
   directory and invokes ``tj3`` (``tj3.c``).  Compile-commands
   projects run the full scheduler; orphan editor-only documents run
   ``tj3 --check-syntax`` instead.
3. Parses ``tj3``'s stderr back into LSP ``Diagnostic`` values
   anchored to the original document URIs.
4. Publishes the diff against its last result via
   ``diag_set_publish()``.

Workers **coalesce**: while a worker is busy, a newer snapshot replaces
its pending request rather than queuing, so a burst of edits collapses
into as few ``tj3`` invocations as possible while still guaranteeing
the latest snapshot is eventually validated. A slow project never
delays another project's diagnostics. The registry itself is owned by
the coordinator and is not thread-safe; ``diag_registry_update()`` and
``diag_registry_shutdown()`` are coordinator-only.

Server-level configuration warnings
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

A workspace without a usable ``compile_commands.json`` cannot drive
cross-file features. Rather than dropping a transient
``window/showMessage`` (which clients hide quickly and which has no
durable home in the UI), the server emits a per-file diagnostic on
every editor-managed document explaining the degradation. This is
collected by ``diag_collect_cc_missing()`` and merged into the same
``diag_set`` that the tj3 worker is about to publish — so the warning
appears wherever the editor opens a ``.tjp`` / ``.tji`` file under the
broken workspace and disappears once the file is fixed.

The compile-commands status itself (``CC_STATUS_OK`` /
``CC_STATUS_MISSING`` / ``CC_STATUS_MALFORMED``) is published on the
``workspace_snapshot`` so any worker can read it without re-touching
disk.

Aggregation (``diag_set``)
--------------------------

``diag_set`` is an ordered URI → ``Diagnostic[]`` map that any
producer can append to (``diag_set_add()``) and whose merged contents
are flushed in one publish per URI by ``diag_set_publish()``. Passing
the previously published ``diag_set`` to ``diag_set_publish()`` lets
it emit an empty notification for every URI whose diagnostics
disappeared between revisions, so stale markers always clear.

Each producer owns its own ``diag_set`` lifetime:

* The coordinator builds and publishes a parser ``diag_set`` after
  every revalidation cycle.
* Each tj3 worker builds a ``diag_set`` per scheduler run and keeps
  the previous one alive until the next publish so the diff can clear
  vanished diagnostics. On worker shutdown, ``clear_published()``
  publishes empties for every URI the worker had outstanding so the
  editor's view returns to "no markers from this source".

The ``source`` field on each ``Diagnostic`` identifies the producer
(``"taskjuggler-lsp"`` for parser and server-level diagnostics,
``"tj3"`` for scheduler diagnostics), so editors can group or filter
markers by origin.

API reference
-------------

.. doxygenfile:: diagnostics.h
.. doxygenfile:: diag_worker.h
.. doxygenfile:: tj3.h
