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
Currently `msvc` support is limited to the release build of the
program. PDBs will be avialable but may be of limited use. `clang`
is fully supported and recommended (and also produces better code).
