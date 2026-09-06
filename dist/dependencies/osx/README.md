# macOS Intel SDL prefix

`intel/` is a committed x86_64 SDL 1.2 install prefix (sdl12-compat over
sdl2-compat over SDL3). CI and `make ARCH=amd64` link against it instead
of building Intel Homebrew under Rosetta.

Rebuild on Apple Silicon when bumping SDL, then commit the prefix:

```
./rebuild-intel-sdl.sh
git add intel
```

Scratch source/build trees land in `.build/` (gitignored).
