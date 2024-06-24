#!/bin/sh
cflags="-march=native -ggdb -O0 -Wall"
ldflags="-lraylib"

case "$1" in
"win32")
	cflags="$cflags -I./external/include"
	ldflags="$ldflags -lgdi32 -lwinmm -L./external"
	;;
*)
	ldflags="$ldflags -lGL"

	# Hot Reloading/Debugging
	cflags="$cflags -D_DEBUG"

	libcflags="$cflags -fPIC -flto -Wno-unused-function"
	libldflags="$ldflags -shared"

	cc $libcflags beamformer.c -o beamformer.so $libldflags
	;;
esac


cc $cflags -o ogl main.c $ldflags
