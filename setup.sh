#!/bin/bash
# One-time: fetch cubiomes into ./cubiomes if not present.
set -e
if [ ! -f cubiomes/finders.h ]; then
  git clone --depth 1 https://github.com/Cubitect/cubiomes.git
fi
echo "Ready. Now run: make cpu   (or)   make gpu"
