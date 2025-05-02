#!/bin/sh


copy_if_exists() {
    local src="$1"
    local dest_dir="$2"

    if [[ -f "$src" ]]; then
        cp "$src" "$dest_dir" \
          && echo "Copied $(basename "$src") to $dest_dir"
    else
        echo "ERROR: '$src' not found."
        exit 1
    fi
}

cflags="-march=native -Wall -std=c11 -I ./external/include"
ldflags="-lm -lgdi32 -lwinmm -lSynchronization -lcuda_toolkit"
output_path="../x64"
app_name="ogl_beamforming_cuda"

cc=${CC:-cc}

# Check for -d flag (debug mode)
if [ "$1" = "-d" ]; then
    cflags="$cflags -O0 -D_DEBUG -Wno-unused-function -fuse-ld=lld -g -gcodeview"
    c_lib_flags="$cflags -Wl,--pdb=beamformer.pdb"
    cflags="$cflags -Wl,--pdb=${app_name}.pdb"
    ldflags="-L ../x64/Debug -L. -lglfw -lraylib ${ldflags}"
    output_path="${output_path}/Debug/"
    ${cc} ${c_lib_flags} -fPIC -shared beamformer.c -o beamformer.dll ${ldflags}
    echo "Building debug OGL"
else
    cflags="$cflags -O3"
    raylib="external/lib/libraylib.a"
	glfw="external/lib/libglfw.a"
	ldflags="-L ../x64/Release ${raylib} ${glfw} ${ldflags}"
    output_path="${output_path}/Release/"
    echo "Building release OGL"
fi
output="${output_path}${app_name}"

cuda_toolkit_path="${output_path}/cuda_toolkit.dll"

${cc} $cflags -o $output main_w32.c $ldflags

copy_if_exists $cuda_toolkit_path "./external/"