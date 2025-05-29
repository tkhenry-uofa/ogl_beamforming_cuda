# ogl beamforming

# Building

Bootstrap the build tool once and it will rebuild itself as
needed:
```sh
cc -march=native -O3 build.c -o build
```

Then simply run the build tool:
```sh
./build
```

## Debug Builds
Simply pass the build tool the `--debug` flag to get a build
suitable for development/debugging:
```
./build --debug
```

### w32
Currently the program is not expected to be buildable with `msvc`.
In order to use Windows debuggers on Windows you will need to
build with `clang`.
