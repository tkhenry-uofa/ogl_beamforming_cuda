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
	glfw="libglfw.dll"
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
	glfw="libglfw.so.3"
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

build_raylib()
{
	rm -r ${2} 2>/dev/null
	cmake --install-prefix="${PWD}/external" \
		-G "Ninja" -B ${2} -S external/raylib \
		${1} \
		-D CMAKE_INSTALL_LIBDIR=lib -D CMAKE_BUILD_TYPE="Release" \
		-DCUSTOMIZE_BUILD=ON -DBUILD_EXAMPLES=OFF -DWITH_PIC=ON \
		-DUSE_EXTERNAL_GLFW=ON \
		-DOPENGL_VERSION=4.3 -DUSE_AUDIO=OFF -DSUPPORT_MODULE_RAUDIO=OFF
	cmake --build   ${2}
	cmake --install ${2}
}

check_and_rebuild_libs()
{
	switch=OFF
	[ ${1} = "shared" ] && switch=ON
	if [ "./build.sh" -nt "${raylib}" ] || [ ! -f "${raylib}" ]; then
		build_raylib "-D BUILD_SHARED_LIBS=${switch}" "external/raylib/build_${1}"
		[ ${1} = "shared" ] && cp -L "${raylib_out_lib}" "${raylib}"
	fi

	# NOTE(rnp): we need to build this separately so that we can use functions from
	# glfw directly - raylib doesn't let us open multiple opengl contexts even if
	# we never plan on using them with raylib
	case "${1}" in
	static)
		if [ "./build.sh" -nt "${glfw}" ] || [ ! -f ${glfw} ]; then
			${cc} ${cflags} -static -D_GLFW_X11 \
				-c external/raylib/src/rglfw.c -o external/lib/rglfw.o
			ar qc ${glfw} external/lib/rglfw.o
		fi
		;;
	shared)
		if [ "./build.sh" -nt "${glfw}" ] || [ ! -f "${glfw}" ]; then
			${cc} ${cflags} -fPIC -shared -D_GLFW_X11 \
				external/raylib/src/rglfw.c -o ${glfw}
		fi
		;;
	esac
}

case "${build}" in
debug)
	cflags="${cflags} -O0 -D_DEBUG -Wno-unused-function"
	if [ "${win32}" ]; then
		# NOTE(rnp): export pdb on win32; requires clang
		cflags="${cflags} -fuse-ld=lld -g -gcodeview -Wl,--pdb="
	else
		cflags="${cflags} -ggdb -Wl,-rpath,."
	fi
	ldflags="-L. -lglfw -lraylib ${ldflags}"
	check_and_rebuild_libs "shared"
	${cc} ${cflags} -fPIC -shared beamformer.c -o ${libname} ${ldflags}
	;;
release)
	cflags="${cflags} -O3"
	raylib="external/lib/libraylib.a"
	glfw="external/lib/libglfw.a"
	ldflags="${raylib} ${glfw} ${ldflags}"
	check_and_rebuild_libs "static"
	;;
esac

${cc} ${cflags} -o ogl ${main} ${ldflags}
