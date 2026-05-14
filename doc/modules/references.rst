References
==========

``references.c`` builds the ``textDocument/references`` response.

Overview
--------

Find-references is answered from ``ref_links[]`` arrays on
``DocSymbol``\ s. Each ``KW_TASK`` node has a ``selection_range``
covering its declaration identifier, and each node may carry
``ref_links[]`` pointing to the source locations of incoming
dependency references.

At query time, ``build_references_json()``:

1. Uses ``symbol_at()`` to locate the innermost ``DocSymbol`` at the
   cursor, walks up to find a ``KW_TASK`` whose ``selection_range``
   contains the cursor. Returns null if none.

2. Iterates the target task's ``ref_links[]`` to collect all incoming
   references (same-document and cross-document).

3. Returns a JSON array of ``Location`` objects, one per reference.
   The array may be empty if no dependency references point to the
   task.

Trigger constraint
------------------

Only task declaration identifiers (``KW_TASK selection_range``)
trigger a response. Positioning the cursor on a reference in a
``depends``/``precedes`` clause, on a keyword, or on a non-task symbol
returns null.

API reference
-------------

.. doxygenfile:: references.h
