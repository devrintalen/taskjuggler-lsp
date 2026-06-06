#!/bin/sh
# Doxygen INPUT_FILTER: strip C11 _Atomic qualifiers from declarations
# before Doxygen sees them.  Doxygen handles the keyword itself, but it
# propagates verbatim into the XML output that Breathe consumes — and
# Breathe's Sphinx C-domain parser then rejects the resulting
# declarations.  Filtering both `_Atomic(T)` and `_Atomic T` forms out
# of the input keeps the published reference clean without touching
# the headers.
sed -E 's/_Atomic\(([^)]*)\)/\1/g; s/_Atomic[[:space:]]+//g' "$1"
