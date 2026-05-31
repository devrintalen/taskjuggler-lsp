Concurrency model
=================

The server processes JSON-RPC messages on several threads while
preserving the LSP "process in arrival order" rule and serving
read-only queries in parallel. This page explains the thread
architecture, the split between the *mutable live store* and the
*immutable published snapshots*, and how the core data types —
``Document``, ``doc_snapshot``, project trees, ``workspace_snapshot``,
and ``query_context`` — relate to one another and to those threads.

For the per-method handler routing see :doc:`server`; for the bare
struct and function signatures see the :doc:`../api` reference.

Threads
-------

There are three kinds of thread:

``reader`` (the ``main`` thread)
    Reads ``Content-Length``-framed messages off stdin one at a time
    (``main.c``), classifies each, and pushes a ``Job`` onto a single
    arrival-ordered queue (``work_queue``). It never touches the
    document store. ``$/cancelRequest`` and ``exit`` are handled here
    directly.

``coordinator`` (one thread)
    Pops jobs from ``work_queue`` **in arrival order** — this single
    serialisation point is what enforces the LSP ordering guarantee
    across queries and notifications alike. The coordinator owns the
    live document store: it runs notifications and the inline lifecycle
    methods (``initialize`` / ``shutdown``) itself, under
    ``docs_mutex``. For every other (read-only) request it pins the
    current published snapshot into the job's ``query_context`` and
    hands the job to the worker pool.

``query workers`` (a fixed pool, ``NUM_QUERY_WORKERS``)
    Pop query jobs from an internal ``request_queue`` and run the
    matching ``handle_*`` against the job's pinned snapshot with **no
    lock held**, then write the response. Because the snapshot is
    immutable, any number of workers run in parallel without
    coordinating.

.. code-block:: text

   stdin
     │  (reader / main thread)
     ▼
   work_queue ───────────────► coordinator (single thread, owns docs[])
     (arrival order)             │
                                 ├─ notification?  run under docs_mutex,
                                 │    re-parse, then publish a new snapshot
                                 ├─ initialize/shutdown?  run inline
                                 └─ other query?  pin snapshot into the
                                      job, push to request_queue
                                          │
                                          ▼
                                    request_queue ──► query worker ×N
                                                        (lock-free, parallel)
                                                        │
                                                        ▼
                                              lsp_send_message (stdout_mutex)

Two mutexes guard shared state: ``docs_mutex`` serialises all access to
the live ``docs[]`` store and snapshot publication (only the
coordinator takes it, so it is effectively a correctness fence rather
than a contention point), and ``stdout_mutex`` serialises response
writes so concurrent workers do not interleave bytes on stdout.

Live store versus published snapshots
-------------------------------------

The design separates **mutable state the coordinator edits** from
**immutable state queries read**:

* The **live store** (``docs[]`` plus the published-snapshot pointer
  ``g_ws``) is touched only by the coordinator thread. Notifications
  mutate it; query coordination reads it to pin a snapshot. Because it
  is single-threaded, the ``g_ws`` pointer itself needs no atomics —
  only the snapshot *refcounts* are atomic, for the worker-side
  release.

* A **snapshot** is immutable once published. A query pins the current
  snapshot with one atomic refcount bump (an O(1) "clone") and reads it
  lock-free. A concurrent notification builds a *new* snapshot and
  atomically swaps ``g_ws``; the old snapshot is freed only when its
  last in-flight reader releases it. A query therefore always observes
  one self-consistent revision for its whole duration.

The data types
--------------

``Document`` — live, mutable slot
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

One per open or background file, in the static ``docs[]`` array, owned
by the coordinator. Holds the editor's working text, the include
prefixes applied to it, the resolved URIs of its ``include`` directives
(``included_uris[]``), a monotonic ``doc_version`` counter, and
pointers to its current and previous ``doc_snapshot``. Everything here
is mutable and only ever read/written by the coordinator under
``docs_mutex``.

``doc_snapshot`` — immutable, refcounted, per parse
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

One document's frozen parse output: the ``tj_node`` tree, the token
spans, a copy of the source text, the parse version stamp, and a
write-once memo for its semantic-token data. Created once per parse and
**shared by reference**: editing document *A* mints a new
``doc_snapshot`` for *A*, while every other document's snapshot is
re-referenced unchanged rather than copied. Each ``Document`` keeps its
current ``snap`` plus the immediately previous ``prev_snap`` (retained
so ``semanticTokens/delta`` can diff against the version the client last
held). A snapshot is freed when the ``Document`` no longer points at it
**and** no live ``workspace_snapshot`` or query still references it —
the refcount tracks exactly that.

projects and ``ws_project`` — the cross-file resolution surface
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

A *project* is the unit of cross-file scoping: each
``compile_commands.json`` entry seeds one project whose membership is
its transitive ``include`` closure, and any document not reached by a
closure becomes its own singleton project. Membership keeps two
unrelated ``.tjp`` files in the same workspace independent.

Each project materialises as a ``ws_project``: a synthetic
``ProjectNode`` root over **all** kinds (tasks, accounts, resources,
reports — a node's ``keyword`` identifies its kind), built by
deep-copying every member document's top-level declarations under the
includer's prefix target (``project_node_from_tj``). This tree, not the
per-document ``tj_node`` trees, is the surface that ``definition``,
``references``, and dependency ``hover`` resolve against, because it is
prefix-applied and carries the dependency edges. It owns no document
memory — every string is copied — so it stays valid independently of
the documents it was built from.

``workspace_snapshot`` — immutable, refcounted, per revision
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

The published cross-file view, built fresh by
``build_workspace_snapshot()`` after every document-changing
notification and swapped into ``g_ws``. It holds:

* a ``ws_doc`` per parsed document — a **reference** to that document's
  current ``doc_snapshot`` plus the include-prefixes in force this
  revision and the index of the project that claimed it during the
  include BFS; and
* a ``ws_project`` per assembled project.

Building a new revision is therefore cheap relative to its scope: the
per-document parse trees are shared by reference (one new
``doc_snapshot`` for the edited document, the rest re-referenced), and
only the project resolution trees are rebuilt — the same per-revision
cost the server already paid.

``query_context`` — a single query's pinned view
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

Built by the coordinator under ``docs_mutex`` and attached to the
``Job``. It holds **one reference** on the current ``workspace_snapshot``
(which transitively pins every ``doc_snapshot`` and project tree it
needs) and, for ``semanticTokens/delta``, one reference on the primary
document's previous ``doc_snapshot``. Its ``query_doc`` entries are
lightweight *views* — borrowed pointers into the pinned snapshot, owning
nothing. ``query_context_free`` (run by the worker after the response)
drops those two references; the snapshot is reclaimed when the last
such reference goes.

Relationships at a glance
~~~~~~~~~~~~~~~~~~~~~~~~~~~

.. code-block:: text

   docs[] (live, coordinator-only)          g_ws (published, immutable)
   ┌───────────────┐                        ┌────────────────────────────┐
   │ Document      │                        │ workspace_snapshot         │
   │  uri, text    │                        │  ws_doc[]                  │
   │  prefixes     │      ref               │   ├─ snap ───────┐         │
   │  snap ───────────────────────┐         │   ├─ prefixes    │ ref     │
   │  prev_snap ──────────┐       │         │   └─ project_index│        │
   └───────────────┘      │       │         │  ws_project[]    │         │
                          │       │         │   └─ ProjectNode tree      │
                          ▼       ▼         │        (deep-copied,       │
                    doc_snapshot (immutable,│         atomic dep memos)  │
                    refcounted; shared by   └─────────────┬──────────────┘
                    every snapshot + query        ref     │ 1 ref
                    that needs it)                        ▼
                          ▲                         query_context (per query)
                          └───── 1 ref (prev_snap, for delta) ──┘
                                                  query_doc[] = borrowed views

Lifecycle of one edit
---------------------

#. The coordinator pops a ``didChange`` (under ``docs_mutex``), applies
   the text edit to the ``Document``, re-parses, and installs a new
   ``doc_snapshot`` — rotating the old current snapshot into
   ``prev_snap`` and stamping the new one with the next ``doc_version``.
#. ``revalidate_all_docs()`` calls ``build_workspace_snapshot()`` to
   assemble a fresh ``workspace_snapshot`` from the current ``docs[]``
   (referencing each document's current ``doc_snapshot``, deep-copying
   the project trees), atomically swaps it into ``g_ws``, and releases
   the previous snapshot.
#. A query that pinned the previous snapshot keeps reading it until it
   releases its reference; that snapshot — and any ``doc_snapshot`` only
   it still referenced — is freed at that point.

Memoization under sharing
-------------------------

Because snapshots are shared across parallel workers, the two lazily
computed caches are published lock-free and write-once:

Dependency resolution
    Each ``project_dep`` memoizes its resolved target in a single atomic
    word (``project_dep_resolve``): ``0`` = unresolved, ``1`` = resolved
    to no target, otherwise the resolved ``ProjectNode``. Resolution is
    a pure function of the immutable tree, so if several workers resolve
    the same cold cell concurrently they compute identical results; the
    idempotent release store is correct whichever lands last (readers
    load it with acquire ordering). Back-to-back queries on one revision
    progressively warm these cells.

Semantic-token data
    Each ``doc_snapshot`` holds a ``sem_token_data`` memo published by
    compare-exchange. The first request computes the encoded buffer;
    concurrent first requests each compute one and the losers free their
    buffer and adopt the winner's. The ``resultId`` returned to the
    client is simply the document's parse version, so a delta diffs by
    version against the retained previous ``doc_snapshot`` with no
    write-back — which is what lets ``semanticTokens`` run on a worker
    like any other query.

API reference
-------------

.. doxygenfile:: query_context.h
.. doxygenfile:: project_tree.h
