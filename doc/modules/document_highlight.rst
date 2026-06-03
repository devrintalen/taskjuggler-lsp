Document highlight
==================

``document_highlight.c`` builds the ``textDocument/documentHighlight``
response.

Overview
--------

Document-highlight binds against two surfaces from the pinned
``query_context``:

* the document's ``TokenSpan`` array, used to identify the token
  under the cursor and to find per-segment ranges within dotted
  reference paths;
* the requesting project's ``ProjectNode`` tree, used to find the
  declaration corresponding to the cursor and to walk its incoming
  dependency edges back to source ranges within the same document.

At query time, ``build_document_highlight_json()``:

1. Finds the ``TK_IDENT`` token at the cursor position. If the cursor
   is not on an identifier, the response is null.

2. Resolves the target declaration. Two paths converge here:

   * Cursor on a **declaration site** (the identifier in a
     ``task foo {…}`` header) — the surrounding ``tj_node`` is the
     target, mapped to its ``ProjectNode`` counterpart in the
     ``ws_project`` tree.

   * Cursor on a **reference site** (inside a ``depends`` /
     ``precedes`` path) — the relevant ``Dependency`` is resolved via
     ``project_dep_resolve()`` to its target ``ProjectNode``. Dotted
     paths are resolved segment-by-segment so each segment can
     highlight independently.

3. Collects highlights for the *current document only*:

   * the target's own ``selection_range`` is emitted as ``Write`` (kind 3);
   * every ``Dependency`` in the project tree whose resolved target is
     this declaration **and** whose source URI matches the requesting
     document contributes a ``Read`` (kind 2) highlight at the
     dependency's source range (see ``collect_read_highlights()``).

Bidirectional triggering
------------------------

Unlike ``textDocument/references``, which only triggers from
declaration sites, ``documentHighlight`` works from both directions:
cursor on a declaration highlights its references in the file, and
cursor on a reference highlights the declaration and all sibling
references in the same file. Cross-file references are intentionally
excluded — ``documentHighlight`` is scoped to one document by the LSP
spec; cross-file results belong to ``textDocument/references``.

API reference
-------------

.. doxygenfile:: document_highlight.h
