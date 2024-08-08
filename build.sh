#!/bin/sh

git submodule update --init
echo "Building trace library..."
(cd tracelib; make "$@" depend && make "$@" && make install)

echo "Building actsim..."
make "$@" depend && make "$@" && make install
