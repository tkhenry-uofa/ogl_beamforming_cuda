# ogl beamforming

# Building

Bootstrap the build tool once and it will rebuild itself as
needed:
```sh
cc -march=native -O3 build.c -o build
```
or:
```bat
md out & cl -nologo -std:c11 -O2 -Fo:out\ build.c
```

Then run the build tool:
```sh
./build
```

## Debug Builds
Pass the build tool the `--debug` flag to get a build suitable for
development/debugging:
```
./build --debug
```

Debug builds enable dynamic reloading of almost the entire program
and you can make changes to most code and recompile without
exiting the application.
