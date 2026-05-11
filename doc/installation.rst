Installation
============

Dependencies
------------

- yyjson_
- Flex_
- Bison_
- Python_ 3 (for the test harness)
- Valgrind_ (optional, for profiling)

On Debian / Ubuntu::

   apt install libyyjson-dev flex bison

On Gentoo::

   emerge -a dev-libs/yyjson sys-devel/flex sys-devel/bison

Building from source
--------------------

Clone the repository and run ``make``::

   git clone https://github.com/devrintalen/taskjuggler-lsp.git
   cd taskjuggler-lsp
   make

This produces the ``taskjuggler-lsp`` binary in the project root.

To clean build artifacts::

   make clean

.. _yyjson: https://github.com/ibireme/yyjson
.. _Flex: https://github.com/westes/flex
.. _Bison: https://www.gnu.org/software/bison/
.. _Python: https://www.python.org/
.. _Valgrind: https://valgrind.org/
