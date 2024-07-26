#!/bin/sh
cflags="-march=native -ggdb -O0 -Wall -I./external/include"
ldflags="-lraylib"

case "$1" in
"win32")
	ldflags="$ldflags -lgdi32 -lwinmm -L./external"
	cc $cflags -o ogl main.c $ldflags
	;;
"win32lib")
	libcflags="$cflags -fPIC -shared"
	cc $libcflags -I'C:/Program Files/MATLAB/R2022a/extern/include' \
		-L'C:/Program Files/MATLAB/R2022a/extern/lib/win64/microsoft' \
		-llibmat -llibmex \
		helpers/ogl_beamformer_lib.c -o helpers/ogl_beamformer_lib.dll
	;;
"lib")
	libcflags="$cflags -I"/opt/matlab/extern/include" -shared -fPIC"
	cc $libcflags helpers/ogl_beamformer_lib.c -o helpers/ogl_beamformer_lib.so
	;;
*)
	ldflags="$ldflags -lGL"

	# Hot Reloading/Debugging
	cflags="$cflags -D_DEBUG -Wno-unused-function"
	#cflags="$cflags -fsanitize=address,undefined"

	libcflags="$cflags -fPIC -Wno-unused-function"
	libldflags="$ldflags -shared"

	cc $libcflags beamformer.c -o beamformer.so $libldflags
	cc $cflags -o ogl main.c $ldflags
	;;
esac
