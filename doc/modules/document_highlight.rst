Document highlight
==================

``document_highlight.c`` builds the ``textDocument/documentHighlight``
response.

Overview
--------

Document-highlight is answered from three data structures in
``ParseResult``:

- ``doc_symbols`` — the symbol tree; each node has a
  ``selection_range`` covering its declaration identifier, an ``id``
  field with the identifier text, and ``def_links[]`` with resolved
  references.
- ``tok_spans`` — flat ordered token array used to identify the token
  at the cursor and to find per-segment ranges within dotted paths.

At query time, ``build_document_highlight_json()``:

1. Finds the ``TK_IDENT`` token at the cursor position.

2. Resolves the target symbol — either the cursor is on a definition
   site (matched via ``doc_symbols``) or on a reference site (matched
   via ``def_links`` on the enclosing symbol). For dotted paths, each
   segment is resolved independently.

3. Collects highlights: the definition as Write (kind 3) and all
   same-document references as Read (kind 2).

Bidirectional triggering
------------------------

Unlike ``textDocument/references``, which only triggers from
definition sites, ``documentHighlight`` works from both directions:
cursor on a definition highlights its references, and cursor on a
reference highlights the definition and all sibling references.

API reference
-------------

.. doxygenfile:: document_highlight.h
