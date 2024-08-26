#!/bin/sh
echo $(pwd)
cflags="-march=native -g -O0 -Wall -I ./external/include"
ldflags="-lraylib  -lgdi32 -lwinmm -lcuda_toolkit -L ./external/lib -L ../x64/Debug"
cc $cflags -o ../x64/Debug/ogl_beamforming_cuda main.c $ldflags


