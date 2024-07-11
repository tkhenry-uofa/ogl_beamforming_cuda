#!/bin/sh
cflags="-march=native -ggdb -O0 -Wall"
ldflags="-lraylib"

case "$1" in
"win32")
	cflags="$cflags -I./external/include"
	ldflags="$ldflags -lgdi32 -lwinmm -L./external"
	;;
"lib")
	cflags="$cflags -I"/opt/matlab/extern/include" -shared -fPIC"
	cc $cflags helpers/ogl_beamformer_lib.c -o helpers/ogl_beamformer_lib.so
	;;
*)
	ldflags="$ldflags -lGL"

	# Hot Reloading/Debugging
	cflags="$cflags -D_DEBUG -Wno-unused-function"

	libcflags="$cflags -fPIC -flto -Wno-unused-function"
	libldflags="$ldflags -shared"

	cc $libcflags beamformer.c -o beamformer.so $libldflags
	;;
esac

cc $cflags -o ogl main.c $ldflags
