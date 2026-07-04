Workspace loading
=================

This page covers how the server discovers, loads, and re-loads
documents in a workspace — from the moment ``initialize`` arrives
through to a steady-state ``workspace_snapshot`` containing one
``ws_project`` per top-level project plus every transitively
included file.

For the threading model that publishes those snapshots, see
:doc:`concurrency`; for the resolution surface they expose, see
:doc:`definition` and :doc:`references`.

``compile_commands.json`` as the entry point
--------------------------------------------

The workspace root (the ``rootUri`` passed to ``initialize``) must
contain a ``compile_commands.json``. This file is the **sole startup
populator** of the document store: every entry's ``file`` field
identifies a top-level ``.tjp`` that the server loads as a project
root.  The transitive ``include`` closure of each root is loaded
alongside it.

The expected schema is the standard CDB form ``[{ "directory":
"...", "file": "...", "command": "..." }, …]``.  Only ``file`` is
required for project discovery; ``directory`` (used to anchor a
relative ``file``) and ``command`` (preserved for future scenario /
``-D`` argument support) are read but not currently exercised by
cross-file features.  See :doc:`../installation` for the minimal
example.

If ``compile_commands.json`` is missing or malformed the server
stays alive but loads no documents.  The status is recorded on the
published snapshot (``CC_STATUS_MISSING`` /
``CC_STATUS_MALFORMED``), and ``diag_collect_cc_missing()`` emits a
per-file warning so the user sees the problem inline in any
``.tjp`` / ``.tji`` they open (see :doc:`diagnostics`).

Document slots
--------------

The live document store (``docs[]`` in ``document_store.c``) holds a
fixed array of ``Document`` slots, each owning:

* the canonical ``file://`` URI;
* the authoritative source text — the editor's working copy while
  the document is open, or the on-disk text otherwise
  (``disk_only = 1`` marks the latter);
* the include prefixes (``task_prefix`` / ``resource_prefix`` /
  ``account_prefix`` / ``report_prefix``) applied to this document
  by the ``include`` directive that pulled it in, or ``NULL`` for a
  top-level project root;
* the resolved ``included_uris[]`` of every ``include`` directive
  inside this document;
* its current ``snap`` and (one revision back) ``prev_snap``
  ``doc_snapshot``.

A slot can be in any of three states:

editor-managed
   The editor has the file open.  Its text wins over the on-disk
   copy and updates on every ``didChange``.

disk-only (background)
   Loaded because some other document includes it (or a CDB entry
   points at it), but the editor has not opened it.  Re-reads from
   disk on watcher events; promoted to editor-managed by ``didOpen``.

free
   Unused slot, available for the next admission.

The coordinator owns ``docs[]`` exclusively.  Workers read only the
immutable ``doc_snapshot`` references they pinned via their
``query_context``; see :doc:`concurrency`.

Include resolution and project membership
-----------------------------------------

After every document-changing notification ``build_workspace_snapshot()``
re-runs a breadth-first walk from each CDB-rooted project:

1. Seed the BFS with the CDB root document.  Its ``ws_project.id`` is
   the root's URI.
2. For every included file in the current frontier, resolve the
   ``include`` path (relative to the includer's directory) to a
   canonical URI, admit a slot for it if one does not exist, and
   stamp its ``ws_doc.project_index`` with the current project.
3. Propagate the includer's prefix targets down: a child file
   reached through ``include "lib.tji" { taskprefix "outer" }``
   contributes its top-level tasks under the prefix ``outer``.

Any document not reached by any CDB-rooted BFS becomes its own
*orphan singleton project* — usually a freshly-opened ``.tjp`` that
the user is editing but has not yet listed in
``compile_commands.json``.  Orphans get a ``ws_project`` of their
own so editor features still work on them; they are flagged
``from_compile_commands = 0`` so the tj3 worker uses
``--check-syntax`` instead of full scheduling against them (see
:doc:`diagnostics`).

A document is **never** a member of more than one project.  This is
what keeps two unrelated ``.tjp`` files in the same workspace from
contaminating each other's cross-file resolution.

File-system events
------------------

The server registers a watcher for ``**/*.tjp`` and ``**/*.tji`` in
``handle_initialized``.  Subsequent ``workspace/didChangeWatchedFiles``
events admit, refresh, or retire disk-only slots; events targeting a
file the editor already has open are ignored, because the editor's
text is authoritative.

``workspace/didChangeWatchedFiles`` is also how a newly created
``compile_commands.json`` is picked up: the coordinator polls the
file on every revalidation cycle and rebuilds the workspace whenever
its mtime or content changes.

API reference
-------------

.. doxygenfile:: workspace.h
.. doxygenfile:: document_store.h
.. doxygenfile:: compile_commands.h
.. doxygenfile:: pathutil.h
