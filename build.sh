#!/bin/sh
cflags="-march=native -std=c11 -O3 -Wall -I./external/include"
#cflags="${cflags} -fproc-stat-report"
#cflags="${cflags} -Rpass-missed=.*"
libcflags="$cflags -fPIC -shared"
ldflags="-lm"

debug=${DEBUG}

cc=${CC:-cc}
system_raylib=${USE_SYSTEM_RAYLIB}

case $(uname -s) in
MINGW64*)
	os="win32"
	ldflags="$ldflags -lgdi32 -lwinmm"
	libname="beamformer.dll"
	;;
Linux*)
	os="unix"
	cflags="$cflags -D_DEFAULT_SOURCE"
	libcflags="$libcflags -I/opt/matlab/extern/include"
	libname="beamformer.so"
	;;
esac

if [ "$system_raylib" ]; then
	ldflags="$(pkg-config raylib) $ldflags"
else
	if [ ! -f external/lib/libraylib.a ]; then
		git submodule update --init --depth=1 external/raylib
		cmake --install-prefix="${PWD}/external" \
			-G "Ninja" -B external/raylib/build_static -S external/raylib \
			-D CMAKE_INSTALL_LIBDIR=lib -D CMAKE_BUILD_TYPE="Release" \
			-D BUILD_SHARED_LIBS=OFF \
			-DCUSTOMIZE_BUILD=ON -DBUILD_EXAMPLES=OFF -DWITH_PIC=ON \
			-DOPENGL_VERSION=4.3 -DUSE_AUDIO=OFF -DSUPPORT_MODULE_RAUDIO=OFF
		cmake --build   external/raylib/build_static
		cmake --install external/raylib/build_static

		# NOTE: we also build the dynamic lib for debug purposes
		cmake --install-prefix="${PWD}/external" \
			-G "Ninja" -B external/raylib/build_shared -S external/raylib \
			-D BUILD_SHARED_LIBS=ON \
			-D CMAKE_INSTALL_LIBDIR=lib -D CMAKE_BUILD_TYPE="Release" \
			-DCUSTOMIZE_BUILD=ON -DBUILD_EXAMPLES=OFF -DWITH_PIC=ON \
			-DOPENGL_VERSION=4.3 -DUSE_AUDIO=OFF -DSUPPORT_MODULE_RAUDIO=OFF
		cmake --build   external/raylib/build_shared
		cmake --install external/raylib/build_shared
	fi
fi

# NOTE: this needs to be separate for now in case matlab junk is not available
case "$1" in
*lib)
	case "$os" in
	"win32")
		${cc} $libcflags -I'C:/Program Files/MATLAB/R2022a/extern/include' \
			helpers/ogl_beamformer_lib.c -o helpers/ogl_beamformer_lib.dll \
			-L'C:/Program Files/MATLAB/R2022a/extern/lib/win64/microsoft' \
			-llibmat -llibmex
		;;
	"unix")
		${cc} $libcflags helpers/ogl_beamformer_lib.c -o helpers/ogl_beamformer_lib.so
		;;
	esac
esac

# Hot Reloading/Debugging
if [ "$debug" ]; then
	cflags="$cflags -O0 -ggdb -D_DEBUG -Wno-unused-function"
	#cflags="$cflags -fsanitize=address,undefined"
	ldflags="-L./external/lib -lraylib -Wl,-rpath,external/lib/ $ldflags"

	libcflags="$cflags -fPIC"
	libldflags="$ldflags -shared"
	${cc} $libcflags beamformer.c -o $libname $libldflags
else
	[ ! "$system_raylib" ] && ldflags="./external/lib/libraylib.a $ldflags"
fi

${cc} $cflags -o ogl main.c $ldflags
