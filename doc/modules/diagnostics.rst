Diagnostics
===========

``diagnostics.c`` covers diagnostic accumulation and LSP diagnostic
delivery. It provides:

1. ``push_diagnostic()`` — appends a diagnostic to ``ParseResult``
   during or after parsing. Called from ``grammar.y`` error rules
   (syntax errors) and from ``parser.c:resolve_dep_refs()`` (semantic
   errors).

2. ``publish_diagnostics()`` — sends the
   ``textDocument/publishDiagnostics`` LSP notification to the editor
   so errors and warnings appear inline.

API reference
-------------

.. doxygenfile:: diagnostics.h
