#!/bin/sh

cflags="-march=native -Wall -I ./external/include"
ldflags="-lglfw -lraylib -lgdi32 -lwinmm -lSynchronization -lcuda_toolkit -L ./external/lib"
output_path="../x64"
app_name="ogl_beamforming_cuda"

cc=${CC:-cc}

# Check for -d flag (debug mode)
if [ "$1" = "-d" ]; then
    cflags="$cflags -g -O0 -D_DEBUG -Wno-unused-function -g -gcodeview"
    c_lib_flags="$cflags -Wl,--pdb=beamformer"
    cflags="$cflags -Wl,--pdb=${app_name}"
    ldflags="$ldflags -fuse-ld=lld -L ../x64/Debug"
    output_path="${output_path}/Debug/"
    ${cc} ${c_lib_flags} -fPIC -shared beamformer.c -o beamformer.dll ${ldflags}
    echo "Building debug OGL"
else
    cflags="$cflags -O3"
    ldflags="$ldflags -L ../x64/Release"
    output_path="${output_path}/Release/"
    echo "Building release OGL"
fi
output="${output_path}${app_name}"

${cc} $cflags -o $output main_w32.c $ldflags