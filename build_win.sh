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


output_path="../output"

cc=${CC:-cc}

# Check for -d flag (debug mode)
if [ "$1" = "-d" ]; then

    output_path="${output_path}/Debug/"
    echo "Building debug OGL"
    ./build.exe --debug
else
    output_path="${output_path}/Release/"
    echo "Building release OGL"
    ./build.exe
fi

cuda_toolkit_path="${output_path}/cuda_toolkit.dll"
copy_if_exists $cuda_toolkit_path "./external/"