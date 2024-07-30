#!/bin/sh
cflags="-march=native -O3 -Wall -I./external/include"
ldflags="-lraylib"
debug=${DEBUG}

cc=${CC:-cc}
system_raylib=${USE_SYSTEM_RAYLIB:-$debug}

if [ "$system_raylib" ]; then
	ldflags="-L/usr/local/lib $ldflags"
else
	ldflags="-L./external/lib $ldflags"
	if [ ! -f external/lib/libraylib.a ]; then
		git submodule update --init --depth=1 external/raylib
		cmake --install-prefix="${PWD}/external" \
			-G "Ninja" -B external/raylib/build -S external/raylib \
			-D CMAKE_INSTALL_LIBDIR=lib -D CMAKE_BUILD_TYPE="Release" \
			-DCUSTOMIZE_BUILD=ON -DBUILD_EXAMPLES=OFF -DWITH_PIC=ON \
			-DOPENGL_VERSION=4.3 -DUSE_AUDIO=OFF -DSUPPORT_MODULE_RAUDIO=OFF
		cmake --build   external/raylib/build
		cmake --install external/raylib/build
	fi
fi

case "$1" in
"win32")
	ldflags="$ldflags -lgdi32 -lwinmm"
	${cc} $cflags -o ogl main.c $ldflags
	;;
"win32lib")
	libcflags="$cflags -fPIC -shared"
	${cc} $libcflags -I'C:/Program Files/MATLAB/R2022a/extern/include' \
		helpers/ogl_beamformer_lib.c -o helpers/ogl_beamformer_lib.dll \
		-L'C:/Program Files/MATLAB/R2022a/extern/lib/win64/microsoft' \
		-llibmat -llibmex
	;;
"lib")
	libcflags="$cflags -I"/opt/matlab/extern/include" -shared -fPIC"
	${cc} $libcflags helpers/ogl_beamformer_lib.c -o helpers/ogl_beamformer_lib.so
	;;
*)
	ldflags="$ldflags -lm"
	# Hot Reloading/Debugging
	if [ "$debug" ]; then
		cflags="$cflags -O0 -ggdb -D_DEBUG -Wno-unused-function"
		#cflags="$cflags -fsanitize=address,undefined"

		libcflags="$cflags -fPIC"
		libldflags="$ldflags -shared"
		${cc} $libcflags beamformer.c -o beamformer.so $libldflags
	fi
	${cc} $cflags -o ogl main.c $ldflags
	;;
esac
