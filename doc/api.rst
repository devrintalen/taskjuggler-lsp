API reference
=============

This page is generated from the project's Doxygen-annotated C sources
via Breathe. For runtime overviews — data flow, document lifecycle,
query dispatch, per-feature behaviour — see the :doc:`modules/index`.

Server core
-----------

The header reference for the server core is included on the
:doc:`modules/server` page; the parser and version headers appear
below.

.. doxygenfile:: parser.h
.. doxygenfile:: version.h

LSP features
------------

Headers with a dedicated module page are linked from
:doc:`modules/index`; the remaining headers below are documented here
from their Doxygen comments only.

.. doxygenfile:: completion.h
.. doxygenfile:: document_symbol.h
.. doxygenfile:: folding_range.h
.. doxygenfile:: hover.h
.. doxygenfile:: semantic_tokens.h
.. doxygenfile:: signature.h
.. doxygenfile:: workspace_symbol.h
