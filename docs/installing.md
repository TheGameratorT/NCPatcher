# Installing

Grab a release, or build from source. Either way the binary can live anywhere:
it locates `ncp.h` and its companions relative to itself, so there is no longer
anything to add to `PATH` beyond the binary, and nothing to reboot for.

**Windows**: run the installer and tick *Add NCPatcher to the system PATH*, or
unzip the portable archive anywhere and put that directory on `PATH` yourself.
With [Scoop](https://scoop.sh): `scoop install ncpatcher`.

**Linux and macOS**: unpack the release archive, or install from source:

```sh
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
sudo cmake --install build              # /usr/local/bin + /usr/local/share/ncpatcher
```

`--prefix` puts it somewhere else; the lookup is relative, so a prefix under
`$HOME` works without any further configuration. Packagers will want
`-DNCP_USE_SYSTEM_DEPS=ON`, which turns a missing yaml-cpp into a configure
error rather than a download. See [packaging/README.md](../packaging/README.md).

## Where the SDK files go

`ncp.h`, `ncp_ide.h` and `ncprt.c` (the SDK) are compiled into your ARM code,
not into ncpatcher, so they are installed as program data rather than as host
headers, in `/usr/share/ncpatcher` or beside the binary on Windows. They are
looked for in:

1. `$NCPATCHER_DATA_DIR`, if set
2. `<directory of the binary>/../share/ncpatcher`
3. the directory of the binary, and its `include` subdirectory

The version of `ncp.h` found is checked against the one the binary expects. A
mismatch is an error rather than a warning: a stale header compiles perfectly
well, emits sections nothing is looking for any more, and hands you a ROM with
the patches silently missing.

## Building from source

CMake 3.20 and a C++20 compiler. yaml-cpp is used if the system has it and
fetched if not.

```sh
git clone https://github.com/TheGameratorT/NCPatcher.git
cd NCPatcher
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
```

On Windows, name the generator instead of the build type:
`cmake -B build -G "Visual Studio 17 2022" -A x64`, then
`cmake --build build --config Release`. The binary lands in `build`, with the
SDK files copied beside it so it can be run from there without installing.

Useful switches:

| Switch | Meaning |
|---|---|
| `-DNCP_BUILD_TESTS=ON` | Build the unit tests; run them with `ctest --test-dir build` |
| `-DNCP_PORTABLE_LAYOUT=ON` | Install everything into one flat directory (the default on Windows) |
| `-DNCP_USE_SYSTEM_DEPS=ON` | Require yaml-cpp from the system rather than fetching it |
