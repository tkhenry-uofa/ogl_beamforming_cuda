#!/bin/sh
echo $(pwd)
cflags="-march=native -ggdb -O0 -Wall -I ./ogl_beamforming_cuda/external/include"
ldflags="-lraylib  -lgdi32 -lwinmm -lcuda_toolkit -L ./ogl_beamforming_cuda/external/lib -L ./x64/Debug"
cc $cflags -o x64/Debug/ogl ogl_beamforming_cuda/main.c $ldflags

