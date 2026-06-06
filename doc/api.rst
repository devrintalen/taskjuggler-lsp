API reference
=============

This page is generated from the project's Doxygen-annotated C sources
via Breathe. For runtime overviews — data flow, document lifecycle,
query dispatch, per-feature behaviour — see the :doc:`modules/index`.

Server core
-----------

The runtime overview for the server core, the threading model, the
multi-source diagnostic pipeline, and the cross-file resolution tree
are on the corresponding :doc:`modules/index` pages.  The headers
linked from those pages are reproduced there; the headers below have
no dedicated module page and are documented here only.

Parser
------

.. doxygenfile:: parser.h

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
.. doxygenfile:: semantic_tokens_delta.h
.. doxygenfile:: signature.h
.. doxygenfile:: workspace_symbol.h
.. doxygenfile:: code_lens.h

Version
-------

.. doxygenfile:: version.h
