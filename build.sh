#!/bin/sh

cflags="-march=native -ggdb -O0 -Wall"
ldflags="-lraylib"

# Hot Reloading/Debugging
cflags="$cflags -D_DEBUG"

libcflags="$cflags -fPIC -flto"
libldflags="$ldflags -shared"

cc $libcflags beamformer.c -o beamformer.so $libldflags
cc $cflags -o ogl main.c $ldflags
