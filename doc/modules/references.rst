References
==========

``references.c`` builds the ``textDocument/references`` response.

Overview
--------

Find-references walks the *project resolution tree* for incoming
dependency edges. Unlike the previous design (which kept a
materialised ``ref_links[]`` array on each declaration), the dev
branch resolves incoming references on demand by traversing the
``ws_project`` tree once per query — every dependency edge is already
memoized on its owning ``ProjectDep`` cell, so a second query on the
same snapshot is a tree walk over warm cells.

At query time, ``build_references_json()``:

1. Locates the requesting document's enclosing ``tj_node`` via the
   pinned ``TokenSpan`` array. Walks up to the nearest
   ``KW_TASK`` whose ``selection_range`` contains the cursor; returns
   null if none.

2. Looks up the requested task's corresponding ``ProjectNode`` in the
   ``ws_project`` tree (the cross-file, prefix-applied surface).

3. Recursively walks the project tree (``collect_refs_in_subtree()``).
   For every task node it visits, every ``Dependency`` is resolved
   via ``project_dep_resolve()``; if the resolved target is the
   requested ``ProjectNode``, a ``Location`` is emitted at the
   dependency's source range and source URI.

The result is an array of ``Location`` objects, one per incoming
``depends`` / ``precedes`` reference, including cross-file ones. It
may be empty if no other task depends on the queried task.

Trigger constraint
------------------

Only task declaration identifiers (``KW_TASK selection_range``)
trigger a response. Positioning the cursor on a reference in a
``depends`` / ``precedes`` clause, on a keyword, or on a non-task
symbol returns null. For bidirectional behaviour (cursor on either
side highlights both ends within one document), see
:doc:`document_highlight`.

API reference
-------------

.. doxygenfile:: references.h
