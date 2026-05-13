Server core
===========

``server.c`` is the request-routing layer that sits between the
JSON-RPC transport in ``main.c`` and the per-feature handlers under
``src/``. It also owns the document store and drives cross-file
revalidation.

Data flow
---------

Inbound (editor → server)
~~~~~~~~~~~~~~~~~~~~~~~~~

The editor sends JSON-RPC messages over stdin. ``main()`` reads each
message, strips the ``Content-Length`` header, and calls
``server_process(json_text)``. ``server_process()`` parses the JSON,
dispatches to the matching ``handle_*`` function, serialises the
returned yyjson response with ``yyjson_mut_write()``, and returns the
string to ``main()`` which calls ``lsp_send_message()`` to write it to
stdout.

Document lifecycle
~~~~~~~~~~~~~~~~~~

Every open document is stored as a ``Document`` in the static
``docs[]`` array. The ``Document`` holds the raw source text and a
fully populated ``ParseResult``.

On ``didOpen`` / ``didChange`` / ``didChangeWatchedFiles`` (created or
changed):

.. code-block:: text

   source text
       │
       ▼
   parse(text)                  ← parser.c entry point
       │  runs the flex lexer and bison parser together:
       │    lexer  →  tok_spans[]        (every token, in order)
       │    grammar→  doc_symbols[]      (task/resource/… tree)
       │    grammar→  raw_dep_refs[]     (unresolved dep expressions)
       │    grammar→  diagnostics[0..dep_diag_start-1]  (syntax errors)
       ▼
   ParseResult (stored in Document.parse)
       │
       ▼
   revalidate_all_docs()        ← called for every open document
       │  for each document, gathers doc_symbols[] from all *other* open
       │  documents as extra symbol pools, then calls:
       │
       └─► publish_diagnostics(uri, &parse)              ← diagnostics.c
               serialises diagnostics[] and pushes a
               textDocument/publishDiagnostics notification to the editor

On ``didClose`` / file deleted, ``parse_result_free()`` is called to
release the ``ParseResult``, the ``Document`` slot is cleared, and
``revalidate_all_docs()`` runs so that references from other files to
symbols in the closed file are re-checked.

Query dispatch
~~~~~~~~~~~~~~

The four populated arrays on ``ParseResult`` each feed a different
fan-out of feature handlers:

.. list-table::
   :header-rows: 1
   :widths: 25 75

   * - ``ParseResult`` field
     - Feature handlers that consume it
   * - ``tok_spans[]``
     - hover → ``active_keyword_at()`` (fallback);
       signature_help → ``active_context()``;
       completion → ``build_completions_json()``;
       folding_range → ``build_folding_ranges_json()`` (brackets, comments);
       semantic_tokens → ``build_semantic_tokens_json()``;
       document_highlight → ``build_document_highlight_json()``
   * - ``doc_symbols[]``
     - document_symbol → ``build_document_symbols_json()``;
       workspace_symbol → ``collect_workspace_symbols()``;
       completion → ``build_completions_json()`` (IDs);
       folding_range → ``build_folding_ranges_json()`` (brace blocks);
       document_highlight → ``build_document_highlight_json()``
   * - ``def_links[]`` (on ``DocSymbol``)
     - definition → ``build_definition_json()``;
       hover → resolved-ref hover (primary path)
   * - ``ref_links[]`` (on ``DocSymbol``)
     - references → ``build_references_json()``;
       document_highlight → ``build_document_highlight_json()``
   * - ``diagnostics[]``
     - Pushed proactively via ``publish_diagnostics``; never queried on demand.

Outbound (server → editor)
~~~~~~~~~~~~~~~~~~~~~~~~~~

Query handlers return a ``yyjson_mut_val *`` response built by
``make_response(doc, id, result)``. ``server_process()`` serialises it
to a string and returns it to ``main()``, which writes:

.. code-block:: text

   Content-Length: <N>\r\n\r\n<json>

to stdout via ``lsp_send_message()``. Diagnostic push notifications
follow the same path but are sent directly from
``publish_diagnostics()`` without going through the response/id
machinery.

API reference
-------------

.. doxygenfile:: server.h
