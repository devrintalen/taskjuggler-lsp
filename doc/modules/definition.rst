Definition
==========

``definition.c`` builds the ``textDocument/definition`` response.

Overview
--------

Go-to-definition resolves against the *project resolution tree* —
the per-``ws_project`` ``ProjectNode`` tree assembled by the workspace
snapshot — rather than against per-document parse output. This is
because a dependency reference like ``depends one.gui`` may resolve
to a task declared in a different file: only the assembled tree
knows the merged, prefix-applied namespace.

At query time, ``build_definition_json()``:

1. Walks the requesting document's ``TokenSpan`` array to find the
   token under the cursor and the enclosing ``tj_node``.

2. If the token is part of a ``depends`` or ``precedes`` reference,
   resolves the corresponding ``Dependency`` against the project's
   ``ProjectNode`` tree via ``project_dep_resolve()``. The resolved
   target's ``selection_range`` and ``source_uri`` become the
   returned ``Location``.

3. Otherwise, if the cursor sits on a task / resource / account
   declaration identifier itself, returns the declaration's own
   selection range (definition of a definition is itself).

Dependency resolution is **memoized on the project tree** by
``project_dep_resolve()``: the resolved target is published
write-once into an atomic word on the ``ProjectDep`` cell, so
concurrent workers pinning the same snapshot resolve safely and
back-to-back queries on one revision warm the memo without re-walking
the tree.

Supported references
--------------------

Currently only task dependency references (``depends`` / ``precedes``
paths) produce navigable definition edges. Resource references
(``allocate``, ``responsible``, ``booking``) and account references
are tracked at the AST level but their cross-file resolution is
not wired into definition yet (see the TODO in
``src/project_tree.c``).

API reference
-------------

.. doxygenfile:: definition.h
.. doxygenfile:: dependency.h
