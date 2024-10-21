#!/bin/sh

cflags="-march=native -Wall -I ./external/include"
ldflags="-lraylib -lgdi32 -lwinmm -lcuda_toolkit -L ./external/lib"
output_path="../x64"
app_name="ogl_beamforming_cuda"

# Check for -d flag (debug mode)
if [ "$1" = "-d" ]; then
    cflags="$cflags -g -O0"
    ldflags="$ldflags -L ../x64/Debug"
    output_path="${output_path}/Debug/"
    echo "Building debug OGL"
else
    cflags="$cflags -O3"
    ldflags="$ldflags -L ../x64/Release"
    output_path="${output_path}/Release/"
    echo "Building release OGL"
fi
output="${output_path}${app_name}"

cc $cflags -o $output main_generic.c $ldflags