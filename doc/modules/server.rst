Server core
===========

``server.c`` is the request-routing layer that sits between the
JSON-RPC transport in ``main.c`` and the per-feature handlers under
``src/``. It owns the document store, drives parsing and cross-file
revalidation, and dispatches every LSP request to the right
``handle_*`` function.

The threading architecture — the coordinator / worker split, the
mutable live store versus the immutable refcounted snapshots, and
how ``Document`` / ``doc_snapshot`` / ``ws_project`` /
``workspace_snapshot`` / ``query_context`` relate — is documented on
the :doc:`concurrency` page. This page covers what the *coordinator*
actually does on each message and how the parse output reaches each
feature handler.

Data flow
---------

Inbound (editor → server)
~~~~~~~~~~~~~~~~~~~~~~~~~

The editor sends JSON-RPC messages over stdin. ``main()`` reads each
``Content-Length``-framed message and calls
``server_process(json_text)``. ``server_process()`` parses the
envelope, classifies the message as an LSP notification or a request,
and enqueues a ``Job`` onto the single arrival-ordered ``work_queue``.

The coordinator pops jobs in arrival order and routes each one:

* LSP **notifications** (``didOpen``, ``didChange``, ``didClose``,
  ``didChangeWatchedFiles``, ``didRenameFiles``, …) run inline under
  ``docs_mutex`` via ``server_dispatch_notification()``. Anything that
  changes a document text triggers a re-parse and a workspace-snapshot
  rebuild.

* LSP **requests** go through ``server_coordinate_query()``. The
  lifecycle methods ``initialize`` and ``shutdown`` and the
  whole-document ``semanticTokens/full`` family are dispatched inline
  too, because they either run only once or fast-path against a single
  ``doc_snapshot``. Every other request has its
  :doc:`query_context <concurrency>` cloned under ``docs_mutex`` and
  is handed off to a query worker that runs the handler lock-free
  against the pinned snapshot.

Document lifecycle
~~~~~~~~~~~~~~~~~~

Every open document is stored as a ``Document`` slot in the static
``docs[]`` array. The slot owns:

* the URI and authoritative source text;
* a monotonic ``doc_version`` counter;
* the include prefixes (``task_prefix`` / ``resource_prefix`` /
  ``account_prefix`` / ``report_prefix``) applied to this document
  by its includer (if any);
* the resolved ``included_uris[]`` of any ``include`` directives in
  the document;
* pointers to its current ``snap`` and immediately previous
  ``prev_snap`` ``doc_snapshot``.

On ``didOpen`` / ``didChange`` / ``didChangeWatchedFiles`` (created or
changed):

.. code-block:: text

   source text
       │
       ▼
   parse(text)                  ← parser.c entry point
       │  drives the flex lexer and bison grammar together:
       │    lexer  →  tok_spans[]        (every token, in source order)
       │    grammar→  root tj_node tree  (task/resource/account/… tree)
       │    grammar→  Dependency arrays  (per-node depends/precedes refs)
       │    grammar→  diagnostics[]      (syntax errors)
       │    grammar→  included_files[]   (include directives + prefixes)
       ▼
   ParseOutput
       │  consumed by docsnap_new():
       │    - root, tok_spans, num_tok_spans, num_sem_entries move into a
       │      fresh doc_snapshot (refcount 1, stamped with doc_version);
       │    - uri and text are copied into the snapshot too.
       ▼
   doc_snapshot (immutable; installed as Document.snap;
                 previous current rotates to Document.prev_snap)
       │
       ▼
   revalidate_all_docs()        ← called once per notification
       │  build_workspace_snapshot() assembles a fresh
       │  workspace_snapshot from the current docs[]: every Document
       │  contributes its doc_snapshot by reference, project membership
       │  is recomputed from compile_commands.json + include closures,
       │  and one ws_project ProjectNode tree is deep-copied per
       │  project.  The previous g_ws is released.
       │
       └─► publish_diagnostics_list(uri, …) for every editor-managed
           document (textDocument/publishDiagnostics).  tj3-driven
           diagnostics are published asynchronously by the per-project
           workers in src/diag_worker.c; see :doc:`diagnostics`.

On ``didClose`` / file deleted, the slot is either retired (if no
include closure still needs it) or downgraded to ``disk_only`` so
cross-file references keep working. Either way
``revalidate_all_docs()`` runs to rebuild the workspace snapshot from
the new ``docs[]`` state.

Query dispatch
~~~~~~~~~~~~~~

A query worker reads only its pinned ``query_context``. The
context resolves to two surfaces:

* **Per-document views** (``query_doc[]``) — borrowed pointers into
  the document's pinned ``doc_snapshot``: the ``tj_node`` ``root``,
  the ``TokenSpan`` array, the source text, and the include
  prefixes. This is what *cursor-position* features bind against
  for their own document.

* **The cross-file resolution surface** (``ws_project``) — the
  synthetic ``ProjectNode`` tree assembled from every member document
  under its prefix target. This is what features needing cross-file
  lookups bind against, because it carries the dependency edges and
  is namespace-flat.

The handler-to-surface mapping:

.. list-table::
   :header-rows: 1
   :widths: 28 72

   * - Surface
     - Feature handlers that consume it
   * - ``TokenSpan`` array (``tok_spans``)
     - hover → ``active_keyword_at()`` (fallback);
       signature_help → ``active_context()``;
       completion → ``build_completions_json()``;
       folding_range → ``build_folding_ranges_json()`` (brackets, comments);
       semantic_tokens → ``docsnap_sem_tokens()`` + ``build_semantic_tokens_json_from_buf()``;
       semantic_tokens_delta → ``build_semantic_tokens_delta_json()``;
       document_highlight → ``build_document_highlight_json()``;
       code_lens → ``build_code_lens_json()``;
       dependency hover → ``dependency_at_cursor()``
   * - Per-document ``tj_node`` root
     - document_symbol → ``build_document_symbols_json()`` (writes the
       declaration tree directly from the document's own root);
       completion → identifier lists drawn from in-scope nodes;
       folding_range → brace-block folding driven by node ranges
   * - ``ProjectNode`` tree (``ws_project``)
     - definition → ``build_definition_json()`` (resolves dependency
       references to their target ``ProjectNode`` via
       ``project_dep_resolve()``);
       references → ``build_references_json()`` (walks the project
       tree for incoming dependencies);
       hover → resolved-ref hover for dependency targets;
       workspace_symbol → ``collect_workspace_symbols()``;
       document_highlight → cross-document highlight resolution
   * - ``diagnostics[]`` (on each ``doc_snapshot``)
     - Pushed proactively from the coordinator via
       ``publish_diagnostics_list``; never queried on demand.  See
       :doc:`diagnostics` for the multi-source aggregation that merges
       parser diagnostics with tj3 results and missing-cc warnings.

Outbound (server → editor)
~~~~~~~~~~~~~~~~~~~~~~~~~~

Each ``handle_*`` returns a ``yyjson_mut_val *`` response. Whichever
thread produced it (coordinator for inline methods, worker for pooled
queries) serialises it with ``yyjson_mut_write()`` and writes the
result to stdout via ``lsp_send_message()``. ``stdout_mutex``
serialises those writes so concurrent workers cannot interleave bytes.

Diagnostic publish notifications follow the same path but are emitted
directly by ``publish_diagnostics_list()`` from
``revalidate_all_docs()`` and from the per-project tj3 workers —
without going through the response/id machinery.

API reference
-------------

.. doxygenfile:: server.h
.. doxygenfile:: job_queue.h
.. doxygenfile:: threadpool.h
