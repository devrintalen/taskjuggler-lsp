Definition
==========

``definition.c`` builds the ``textDocument/definition`` response.

Overview
--------

Go-to-definition is answered from ``DefinitionLink`` arrays stored on
each ``DocSymbol``. Each ``DefinitionLink`` records a source range
(the reference expression) and a target pointer (the resolved
``DocSymbol``).

These links are populated by ``parser.c:resolve_dep_refs()`` after
every document change for every successfully resolved dependency
reference.

At query time, ``build_definition_json()`` locates the innermost
``DocSymbol`` containing the cursor via ``symbol_at()`` and scans its
``def_links`` for one whose source range covers the cursor. When
found, it returns a ``Location`` object pointing at the target
symbol's ``selection_range``.

Supported references
--------------------

Currently only task dependency references (``depends`` / ``precedes``
paths) produce definition links. Resource references (``allocate``,
``responsible``, ``booking``) are not yet tracked.

API reference
-------------

.. doxygenfile:: definition.h
