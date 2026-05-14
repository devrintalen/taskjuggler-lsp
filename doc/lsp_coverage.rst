LSP Coverage
============

This page tracks which features of the `Language Server Protocol`_
(version 3.17) are implemented by ``taskjuggler-lsp`` and which are
not.  Each row identifies the LSP request or notification, whether
the server handles it, and a short note describing the current
implementation or, for unimplemented features, the reason it is
absent.

Legend
------

============  ===========================================================
Status        Meaning
============  ===========================================================
Yes           Implemented and advertised in ``initialize`` capabilities.
Partial       Implemented with documented restrictions.
No            Not implemented.  Either intentionally out of scope for
              TaskJuggler or planned but not yet built.
N/A           Not meaningful for TaskJuggler source (e.g. notebook
              document sync, type hierarchies for a language with no
              types).
============  ===========================================================

.. _Language Server Protocol: https://microsoft.github.io/language-server-protocol/specifications/lsp/3.17/specification/

Lifecycle messages
------------------

.. list-table::
   :header-rows: 1
   :widths: 30 12 58

   * - Method
     - Status
     - Notes
   * - ``initialize``
     - Yes
     - Server advertises capabilities for every feature listed below.
       Client capabilities are received but currently not inspected;
       the server behaves as if every advertised capability is
       supported.  ``rootUri`` and ``workspaceFolders`` are both
       honoured and deduplicated; the resulting roots drive the
       initial workspace scan.
   * - ``initialized``
     - Yes
     - On receipt, the server sends a ``client/registerCapability``
       request asking the client to watch ``**/*.tjp`` and
       ``**/*.tji`` so cross-file references stay consistent.
   * - ``shutdown``
     - Yes
     - Returns ``null`` per spec.
   * - ``exit``
     - Yes
     - Terminates the process immediately.
   * - ``$/cancelRequest``
     - No
     - Server is single-threaded and synchronous; every request
       completes before the next message is read, so cancellation has
       no effect.
   * - ``$/progress``
     - No
     - No long-running operations are reported back to the client.

Text document synchronization
-----------------------------

.. list-table::
   :header-rows: 1
   :widths: 30 12 58

   * - Method
     - Status
     - Notes
   * - ``textDocument/didOpen``
     - Yes
     - Promotes the URI from a background entry (if any) to an
       editor-owned slot; the editor's text becomes authoritative.
   * - ``textDocument/didChange``
     - Yes
     - ``TextDocumentSyncKind.Incremental`` (``2``).  Each change
       range is applied against the stored text before re-parsing.
   * - ``textDocument/didClose``
     - Yes
     - Re-reads the file from disk and keeps it as a background
       entry so cross-file references survive editor close.
   * - ``textDocument/willSave``
     - No
     - Not needed; the server has no save-time bookkeeping.
   * - ``textDocument/willSaveWaitUntil``
     - No
     - No formatter or save-time edits are produced.
   * - ``textDocument/didSave``
     - No
     - Editor text is already authoritative; on-disk content is
       reloaded only via watcher events.

Language features
-----------------

.. list-table::
   :header-rows: 1
   :widths: 30 12 58

   * - Method
     - Status
     - Notes
   * - ``textDocument/publishDiagnostics``
     - Yes
     - Pushed after every parse and after every cross-file
       revalidation cycle.  Diagnostics cover syntax errors,
       in-file dependency resolution, and cross-file dependency
       resolution.
   * - ``textDocument/diagnostic`` (pull)
     - No
     - Push diagnostics already cover every editor that observes
       ``publishDiagnostics``; pull-mode has not been needed.
   * - ``textDocument/hover``
     - Yes
     - Returns Markdown content.  Keyword hovers contain the
       documentation excerpt; identifier hovers describe the
       declaration kind.
   * - ``textDocument/completion``
     - Partial
     - Context-aware: scope-sensitive identifier lists for
       dependency arguments, keyword completion elsewhere.
       Triggered on ``!`` and ``.`` in addition to the usual word
       characters.  No ``completionItem/resolve`` — completion items
       are returned fully populated.  No snippet support
       (``insertTextFormat`` is plain text).
   * - ``textDocument/signatureHelp``
     - Yes
     - Triggered on space.  Tracks the active keyword and argument
       index through ``active_context()``.
   * - ``textDocument/definition``
     - Yes
     - Resolves to the declaring symbol.  Cross-file edges are
       followed via the ``DefinitionLink`` produced during
       revalidation.
   * - ``textDocument/declaration``
     - No
     - TaskJuggler does not distinguish declarations from
       definitions; ``definition`` already returns the declaring
       site.
   * - ``textDocument/typeDefinition``
     - N/A
     - TaskJuggler has no type system.
   * - ``textDocument/implementation``
     - N/A
     - No interface / implementation split in the language.
   * - ``textDocument/references``
     - Yes
     - Returns every ``ReferenceLink`` recorded on the target
       symbol, including cross-file references.
   * - ``textDocument/documentHighlight``
     - Yes
     - Highlights every occurrence of the identifier under the
       cursor within the current document.
   * - ``textDocument/documentSymbol``
     - Yes
     - Hierarchical symbols.  Tasks → ``Function``, resources →
       ``Object``, accounts → ``Variable``, shifts → ``Event``.
   * - ``textDocument/codeAction``
     - No
     - No quick-fixes or refactorings have been implemented yet.
   * - ``codeAction/resolve``
     - No
     - Depends on ``codeAction``.
   * - ``textDocument/codeLens``
     - No
     - No code lenses are produced.
   * - ``codeLens/resolve``
     - No
     - Depends on ``codeLens``.
   * - ``textDocument/documentLink``
     - No
     - ``include`` paths are followed internally but not surfaced as
       clickable links.  A future addition is plausible.
   * - ``documentLink/resolve``
     - No
     - Depends on ``documentLink``.
   * - ``textDocument/documentColor``
     - No
     - No colour values are exposed by the language at the
       identifier level.
   * - ``textDocument/colorPresentation``
     - No
     - Depends on ``documentColor``.
   * - ``textDocument/formatting``
     - No
     - No canonical formatter exists for TJP.
   * - ``textDocument/rangeFormatting``
     - No
     - Depends on a formatter implementation.
   * - ``textDocument/onTypeFormatting``
     - No
     - Depends on a formatter implementation.
   * - ``textDocument/rename``
     - No
     - Cross-file renaming of tasks / resources / accounts is
       feasible against the existing reference graph but has not
       been wired up.
   * - ``textDocument/prepareRename``
     - No
     - Depends on ``rename``.
   * - ``textDocument/foldingRange``
     - Yes
     - Brace-block folding driven by the ``doc_symbols[]`` tree.
   * - ``textDocument/selectionRange``
     - No
     - Not implemented.  Would require walking the symbol tree
       outward from the cursor.
   * - ``textDocument/prepareCallHierarchy``
     - N/A
     - No call hierarchy in TJP.
   * - ``callHierarchy/incomingCalls``
     - N/A
     - As above.
   * - ``callHierarchy/outgoingCalls``
     - N/A
     - As above.
   * - ``textDocument/prepareTypeHierarchy``
     - N/A
     - No type system.
   * - ``typeHierarchy/supertypes`` / ``subtypes``
     - N/A
     - As above.
   * - ``textDocument/semanticTokens/full``
     - Yes
     - Legend exposes the token types ``keyword``, ``comment``,
       ``string``, ``number``, ``variable``, ``function`` and the
       single modifier ``declaration``.  The whole-document encoding
       is recomputed on every request; there is no per-document
       cache.  This dominates server CPU on large workspaces
       (see :doc:`performance`).
   * - ``textDocument/semanticTokens/full/delta``
     - No
     - Requires storing a ``resultId`` per document and computing a
       token diff against the previously returned set.  Tracked as a
       TODO in ``src/server.c``.
   * - ``textDocument/semanticTokens/range``
     - No
     - Requires filtering ``tok_spans[]`` to the requested range
       before encoding.  Tracked as a TODO in ``src/server.c``.
   * - ``textDocument/linkedEditingRange``
     - No
     - Not implemented.
   * - ``textDocument/moniker``
     - No
     - No external indexing integration.
   * - ``textDocument/inlayHint``
     - No
     - Not implemented.
   * - ``inlayHint/resolve``
     - No
     - Depends on ``inlayHint``.
   * - ``textDocument/inlineValue``
     - N/A
     - Inline values are a debugger concept; TaskJuggler is not a
       debug target.
   * - ``textDocument/inlineCompletion``
     - No
     - Not implemented.

Workspace features
------------------

.. list-table::
   :header-rows: 1
   :widths: 30 12 58

   * - Method
     - Status
     - Notes
   * - ``workspace/symbol``
     - Yes
     - Returns every top-level declaration across open and
       background documents that matches the query.
   * - ``workspaceSymbol/resolve``
     - No
     - Symbols are returned fully resolved.
   * - ``workspace/didChangeWatchedFiles``
     - Yes
     - File watcher registration is requested in
       ``handle_initialized``.  Watcher events are ignored for files
       the editor already has open (the editor's text is
       authoritative for those).
   * - ``workspace/didChangeWorkspaceFolders``
     - No
     - Workspace roots are read once at ``initialize``.  Adding or
       removing folders mid-session is not yet supported.
   * - ``workspace/didChangeConfiguration``
     - No
     - The server has no user-tunable configuration.
   * - ``workspace/configuration``
     - No
     - Not requested from the client.
   * - ``workspace/willCreateFiles`` / ``didCreateFiles``
     - No
     - File creation is picked up indirectly via
       ``didChangeWatchedFiles``.
   * - ``workspace/willRenameFiles``
     - No
     - Only the post-rename notification is handled.
   * - ``workspace/didRenameFiles``
     - Yes
     - Updates URIs in the document store so reference and
       definition links remain valid after a rename.
   * - ``workspace/willDeleteFiles`` / ``didDeleteFiles``
     - No
     - File removal is picked up indirectly via
       ``didChangeWatchedFiles``.
   * - ``workspace/executeCommand``
     - No
     - No server-defined commands are advertised.
   * - ``workspace/applyEdit``
     - No
     - Server does not produce edits (no rename, code actions, or
       formatter).
   * - ``workspace/diagnostic`` (pull)
     - No
     - Push diagnostics are used instead.

Window and client utility messages
----------------------------------

.. list-table::
   :header-rows: 1
   :widths: 30 12 58

   * - Method
     - Status
     - Notes
   * - ``window/showMessage``
     - No
     - Server never surfaces messages to the user; errors are
       reported as diagnostics or via stderr.
   * - ``window/showMessageRequest``
     - No
     - As above.
   * - ``window/logMessage``
     - No
     - Logging goes to stderr.
   * - ``window/showDocument``
     - No
     - Not used.
   * - ``window/workDoneProgress/create``
     - No
     - No long-running operations are tracked.
   * - ``$/setTrace`` / ``$/logTrace``
     - No
     - Not implemented; tracing is not surfaced.
   * - ``telemetry/event``
     - No
     - No telemetry is emitted.
   * - ``client/registerCapability``
     - Yes
     - Sent once during ``initialized`` to register the file
       watcher.  No other dynamic registrations are issued.
   * - ``client/unregisterCapability``
     - No
     - Registrations are never withdrawn.

Notebook document synchronization
---------------------------------

Not applicable.  TaskJuggler source files are plain text; the server
does not implement any of the ``notebookDocument/*`` requests or
notifications.
