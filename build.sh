#!/bin/sh
# Requires: sudo apt install libgtk2.0-dev libraw-dev
set -e
gcc -O2 -fPIC -shared -Wall -Wextra -Wno-deprecated-declarations \
    -o rawview.wlx rawview.c $(pkg-config --cflags --libs gtk+-2.0 libraw) -lm
