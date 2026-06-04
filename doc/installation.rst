Installation
============

Dependencies
------------

Build-time
~~~~~~~~~~

- yyjson_
- Flex_
- Bison_
- Python_ 3 (test harness and benchmark tooling)
- Valgrind_ (optional, for callgrind profiling)

On Debian / Ubuntu::

   apt install libyyjson-dev flex bison

On Gentoo::

   emerge -a dev-libs/yyjson sys-devel/flex sys-devel/bison

Runtime
~~~~~~~

- TaskJuggler_ ``tj3`` 3.x (optional but strongly recommended)

If ``tj3`` is on ``PATH`` the server invokes it asynchronously per
project to surface real scheduler diagnostics — unresolved cross-file
references, scheduling errors, date-range conflicts.  Without it the
server still parses and serves every LSP feature; only the
``tj3``-sourced diagnostics are absent.  See
:doc:`modules/diagnostics` for the integration.

TaskJuggler ships as a Ruby gem (not a distro package on most
systems), so the canonical install is::

   gem install taskjuggler

On Debian / Ubuntu, install Ruby first via ``apt install ruby``; on
Gentoo, ``emerge dev-lang/ruby``.  ``taskjuggler`` is not in the main
Gentoo portage tree.

Building from source
--------------------

Clone the repository and run ``make``::

   git clone https://github.com/devrintalen/taskjuggler-lsp.git
   cd taskjuggler-lsp
   make

This produces the ``taskjuggler-lsp`` binary in the project root.

To clean build artifacts::

   make clean

The bison-generated header (``src/grammar.tab.h``) is a prerequisite
for every ``.o``, so ``make clean && make`` is the safe way to
recover from a stale generated header.

Workspace configuration
-----------------------

After installation, each workspace needs a ``compile_commands.json``
at its root listing the project's top-level ``.tjp`` files.  This
file is the sole source of truth for which projects exist; without
it the server stays alive but loads no documents and reports the
problem as a per-file diagnostic.

Minimal schema (only ``file`` and optional ``directory`` are read
today; ``command`` is kept for future use)::

   [
     { "directory": ".", "command": "tj3 main.tjp", "file": "main.tjp" },
     { "directory": ".", "command": "tj3 other.tjp", "file": "other.tjp" }
   ]

Each listed ``.tjp`` becomes the root of one project; its transitive
``include`` closure (every reachable ``.tji`` / ``.tjp``) joins the
same project automatically and inherits the includer's prefix.

.. _yyjson: https://github.com/ibireme/yyjson
.. _Flex: https://github.com/westes/flex
.. _Bison: https://www.gnu.org/software/bison/
.. _Python: https://www.python.org/
.. _Valgrind: https://valgrind.org/
.. _TaskJuggler: https://taskjuggler.org/
