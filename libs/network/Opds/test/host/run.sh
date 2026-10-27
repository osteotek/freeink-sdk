#!/bin/sh
# Builds and runs the OPDS feed parser tests: OpdsFeedParser with both backends
# (Atom over the system expat, JSON over JsonSax) against a budgeted heap.
set -e
cd "$(dirname "$0")"
BUILD_DIR="${TMPDIR:-/tmp}/freeink-opds-host-tests"
mkdir -p "$BUILD_DIR"
c++ -std=c++17 -Wall -Wextra -Werror -Istubs -I../../include -I../../../JsonSax/include \
  test_opds_parsers.cpp ../../src/OpdsFeedParser.cpp ../../src/OpdsParser.cpp ../../src/Opds2Parser.cpp \
  ../../src/OpdsRowBudget.cpp \
  ../../../JsonSax/src/StreamingJsonParser.cpp -lexpat -o "$BUILD_DIR/test_opds_parsers"
"$BUILD_DIR/test_opds_parsers"
