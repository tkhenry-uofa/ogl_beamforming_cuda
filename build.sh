#!/bin/sh

cflags="-march=native -std=c11 -Wall -I./external/include"
#cflags="${cflags} -fsanitize=address,undefined"
#cflags="${cflags} -fproc-stat-report"
#cflags="${cflags} -Rpass-missed=.*"
libcflags="${cflags} -fPIC -shared -Wno-unused-variable"
ldflags="-lm"

cc=${CC:-cc}
build=release

for arg in $@; do
	case "$arg" in
	clang)   cc=clang      ;;
	gcc)     cc=gcc        ;;
	debug)   build=debug   ;;
	release) build=release ;;
	*) echo "usage: $0 [release|debug] [gcc|clang]" ;;
	esac
done

case $(uname -sm) in
MINGW64*)
	win32=1
	raylib="libraylib.dll"
	raylib_out_lib="external/bin/libraylib.dll"
	main="main_w32.c"
	libname="beamformer.dll"
	ldflags="${ldflags} -lgdi32 -lwinmm"
	if [ ! ${NO_MATLAB} ]; then
		libcflags="${libcflags} -DMATLAB_CONSOLE"
		extra_ldflags="-llibmat -llibmex"
	fi
	${cc} ${libcflags} helpers/ogl_beamformer_lib.c -o helpers/ogl_beamformer_lib.dll \
		-L'C:/Program Files/MATLAB/R2022a/extern/lib/win64/microsoft' \
		${extra_ldflags}
	;;
Linux*)
	raylib="libraylib.so.550"
	raylib_out_lib="external/lib/libraylib.so.5.5.0"
	main="main_linux.c"
	libname="beamformer.so"
	cflags="${cflags} -D_DEFAULT_SOURCE"

	${cc} ${libcflags} helpers/ogl_beamformer_lib.c -o helpers/ogl_beamformer_lib.so
	;;
esac

if [ ! -f external/raylib/README.md ] || [ "$(git status --short external/raylib)" ]; then
	git submodule update --init --depth=1 external/raylib
fi

case "${build}" in
debug)
	cflags="${cflags} -O0 -D_DEBUG -Wno-unused-function"
	if [ "${win32}" ]; then
		# NOTE(rnp): export pdb on win32; requires clang
		cflags="${cflags} -fuse-ld=lld -g -gcodeview -Wl,--pdb="
	else
		cflags="${cflags} -ggdb -Wl,-rpath,."
	fi
	if [ ! -f "${raylib}" ]; then
		cmake --install-prefix="${PWD}/external" \
			-G "Ninja" -B external/raylib/build_shared -S external/raylib \
			-D BUILD_SHARED_LIBS=ON \
			-D CMAKE_INSTALL_LIBDIR=lib -D CMAKE_BUILD_TYPE="Release" \
			-DCUSTOMIZE_BUILD=ON -DBUILD_EXAMPLES=OFF -DWITH_PIC=ON \
			-DOPENGL_VERSION=4.3 -DUSE_AUDIO=OFF -DSUPPORT_MODULE_RAUDIO=OFF
		cmake --build   external/raylib/build_shared
		cmake --install external/raylib/build_shared
		cp "${raylib_out_lib}" "${raylib}"
	fi
	ldflags="-L. -lraylib ${ldflags}"
	libcflags="${cflags} -fPIC -shared"
	${cc} ${libcflags} beamformer.c -o ${libname} ${ldflags}
	;;
release)
	cflags="${cflags} -O3"
	if [ ! -f external/lib/libraylib.a ]; then
		cmake --install-prefix="${PWD}/external" \
			-G "Ninja" -B external/raylib/build_static -S external/raylib \
			-D CMAKE_INSTALL_LIBDIR=lib -D CMAKE_BUILD_TYPE="Release" \
			-D BUILD_SHARED_LIBS=OFF \
			-DCUSTOMIZE_BUILD=ON -DBUILD_EXAMPLES=OFF -DWITH_PIC=ON \
			-DOPENGL_VERSION=4.3 -DUSE_AUDIO=OFF -DSUPPORT_MODULE_RAUDIO=OFF
		cmake --build   external/raylib/build_static
		cmake --install external/raylib/build_static
	fi
	ldflags="./external/lib/libraylib.a ${ldflags}"
	;;
esac

${cc} ${cflags} -o ogl ${main} ${ldflags}
